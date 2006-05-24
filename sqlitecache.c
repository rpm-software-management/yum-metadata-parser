/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <Python.h>

#include "xml-parser.h"
#include "db.h"
#include "package.h"

/* Make room for 2500 package ids, 40 bytes + '\0' each */
#define PACKAGE_IDS_CHUNK 41 * 2500

typedef void (*ProgressFn) (guint32 current, guint32 total, gpointer user_data);

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *remove_handle;
    guint32 count_from_md;
    guint32 packages_seen;
    guint32 add_count;
    guint32 del_count;
    GHashTable *current_packages;
    GHashTable *all_packages;
    GStringChunk *package_ids_chunk;
    GTimer *timer;
    ProgressFn progress_cb;
    gpointer user_data;
} UpdateInfo;

static void
update_info_init (UpdateInfo *info, GError **err)
{
    const char *sql;
    int rc;

    sql = "DELETE FROM packages WHERE pkgKey = ?";
    rc = sqlite3_prepare (info->db, sql, -1, &info->remove_handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare changelog insertion: %s",
                     sqlite3_errmsg (info->db));
        sqlite3_finalize (info->remove_handle);
        return;
    }

    info->count_from_md = 0;
    info->packages_seen = 0;
    info->add_count = 0;
    info->del_count = 0;
    info->all_packages = g_hash_table_new (g_str_hash, g_str_equal);
    info->package_ids_chunk = g_string_chunk_new (PACKAGE_IDS_CHUNK);
    info->timer = g_timer_new ();
    g_timer_start (info->timer);
    info->current_packages = yum_db_read_package_ids (info->db, err);
}

static void
remove_entry (gpointer key, gpointer value, gpointer user_data)
{
    UpdateInfo *info = (UpdateInfo *) user_data;

    if (g_hash_table_lookup (info->all_packages, key) == NULL) {
        int rc;

        sqlite3_bind_int (info->remove_handle, 1, GPOINTER_TO_INT (value));
        rc = sqlite3_step (info->remove_handle);
        sqlite3_reset (info->remove_handle);

        if (rc != SQLITE_DONE)
            g_warning ("Error removing package from SQL: %s",
                       sqlite3_errmsg (info->db));

        info->del_count++;
    }
}

static void
update_info_remove_old_entries (UpdateInfo *info)
{
    g_hash_table_foreach (info->current_packages, remove_entry, info);
}

static void
count_cb (guint32 count, gpointer user_data)
{
    UpdateInfo *info = (UpdateInfo *) user_data;

    info->count_from_md = count;
}

static void
update_info_done (UpdateInfo *info, GError **err)
{
    if (info->remove_handle)
        sqlite3_finalize (info->remove_handle);
    if (info->current_packages)
        g_hash_table_destroy (info->current_packages);
    if (info->all_packages)
        g_hash_table_destroy (info->all_packages);
    if (info->package_ids_chunk)
        g_string_chunk_free (info->package_ids_chunk);

    g_timer_stop (info->timer);
    if (!*err) {
        g_message ("Added %d new packages, deleted %d old in %.2f seconds",
                   info->add_count, info->del_count,
                   g_timer_elapsed (info->timer, NULL));
    }

    g_timer_destroy (info->timer);
}

/* Primary */

typedef struct {
    UpdateInfo update_info;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *requires_handle;
    sqlite3_stmt *provides_handle;
    sqlite3_stmt *conflicts_handle;
    sqlite3_stmt *obsoletes_handle;
    sqlite3_stmt *files_handle;
} PackageWriterInfo;

static void
package_writer_info_init (PackageWriterInfo *info, sqlite3 *db, GError **err)
{
    info->pkg_handle = yum_db_package_prepare (db, err);
    if (*err)
        return;
    info->requires_handle = yum_db_dependency_prepare (db, "requires", err);
    if (*err)
        return;
    info->provides_handle = yum_db_dependency_prepare (db, "provides", err);
    if (*err)
        return;
    info->conflicts_handle = yum_db_dependency_prepare (db, "conflicts", err);
    if (*err)
        return;
    info->obsoletes_handle = yum_db_dependency_prepare (db, "obsoletes", err);
    if (*err)
        return;
    info->files_handle = yum_db_file_prepare (db, err);
}

static void
write_deps (sqlite3 *db, sqlite3_stmt *handle, gint64 pkgKey, GSList *deps)
{
    GSList *iter;

    for (iter = deps; iter; iter = iter->next)
        yum_db_dependency_write (db, handle, pkgKey, (Dependency *) iter->data);
}

static void
write_files (sqlite3 *db, sqlite3_stmt *handle, Package *pkg)
{
    GSList *iter;

    for (iter = pkg->files; iter; iter = iter->next)
        yum_db_file_write (db, handle, pkg->pkgKey,
                           (PackageFile *) iter->data);
}

static void
write_package_to_db (UpdateInfo *update_info, Package *package)
{
    PackageWriterInfo *info = (PackageWriterInfo *) update_info;

    yum_db_package_write (update_info->db, info->pkg_handle, package);

    write_deps (update_info->db, info->requires_handle,
                package->pkgKey, package->requires);
    write_deps (update_info->db, info->provides_handle,
                package->pkgKey, package->provides);
    write_deps (update_info->db, info->conflicts_handle,
                package->pkgKey, package->conflicts);
    write_deps (update_info->db, info->obsoletes_handle,
                package->pkgKey, package->obsoletes);

    write_files (update_info->db, info->files_handle, package);
}

static void
add_package (Package *package, gpointer user_data)
{
    UpdateInfo *info = (UpdateInfo *) user_data;

    g_hash_table_insert (info->all_packages,
                         g_string_chunk_insert (info->package_ids_chunk,
                                                package->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (info->current_packages,
                             package->pkgId) == NULL) {

        write_package_to_db (info, package);
        info->add_count++;
    }

    if (info->count_from_md > 0 && info->progress_cb)
        info->progress_cb (++info->packages_seen,
                           info->count_from_md,
                           info->user_data);
}

static void
package_writer_info_clean (PackageWriterInfo *info)
{
    if (info->pkg_handle)
        sqlite3_finalize (info->pkg_handle);
    if (info->requires_handle)
        sqlite3_finalize (info->requires_handle);
    if (info->provides_handle)
        sqlite3_finalize (info->provides_handle);
    if (info->conflicts_handle)
        sqlite3_finalize (info->conflicts_handle);
    if (info->obsoletes_handle)
        sqlite3_finalize (info->obsoletes_handle);
    if (info->files_handle)
        sqlite3_finalize (info->files_handle);
}

static char *
update_primary (const char *md_filename,
                const char *checksum,
                ProgressFn progress_cb,
                gpointer user_data,
                GError **err)
{
    PackageWriterInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    memset (&info, 0, sizeof (PackageWriterInfo));

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename, checksum,
                                   yum_db_create_primary_tables,
                                   err);

    if (*err != NULL)
        goto cleanup;

    if (!update_info->db)
        return db_filename;

    update_info_init (update_info, err);
    if (*err)
        goto cleanup;

    update_info->progress_cb = progress_cb;
    update_info->user_data = user_data;
    package_writer_info_init (&info, update_info->db, err);
    if (*err)
        goto cleanup;

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_primary (md_filename, count_cb, add_package, &info, err);
    if (*err)
        goto cleanup;
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    update_info_remove_old_entries (update_info);

    yum_db_dbinfo_update (update_info->db, checksum, err);

 cleanup:
    package_writer_info_clean (&info);
    update_info_done (update_info, err);

    if (update_info->db)
        sqlite3_close (update_info->db);

    if (*err) {
        g_free (db_filename);
        db_filename = NULL;
    }

    return db_filename;
}

/* Filelists */

typedef struct {
    UpdateInfo update_info;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *file_handle;
} FileListInfo;

static void
update_filelist_cb (Package *p, gpointer user_data)
{
    FileListInfo *info = (FileListInfo *) user_data;
    UpdateInfo *update_info = &info->update_info;

    g_hash_table_insert (update_info->all_packages,
                         g_string_chunk_insert (update_info->package_ids_chunk,
                                                p->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (update_info->current_packages,
                             p->pkgId) == NULL) {

        yum_db_package_ids_write (update_info->db, info->pkg_handle, p);
        yum_db_filelists_write (update_info->db, info->file_handle, p);
        update_info->add_count++;
    }

    if (update_info->count_from_md > 0 && update_info->progress_cb)
        update_info->progress_cb (++update_info->packages_seen,
                                  update_info->count_from_md,
                                  update_info->user_data);
}

static char *
update_filelist (const char *md_filename,
                 const char *checksum,
                 ProgressFn progress_cb,
                 gpointer user_data,
                 GError **err)
{
    FileListInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    memset (&info, 0, sizeof (FileListInfo));

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename, checksum,
                                   yum_db_create_filelist_tables,
                                   err);

    if (*err)
        goto cleanup;

    if (!update_info->db)
        return db_filename;

    update_info_init (update_info, err);
    if (*err)
        goto cleanup;
    update_info->progress_cb = progress_cb;
    update_info->user_data = user_data;
    info.pkg_handle = yum_db_package_ids_prepare (update_info->db, err);
    if (*err)
        goto cleanup;

    info.file_handle = yum_db_filelists_prepare (update_info->db, err);
    if (*err)
        goto cleanup;

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_filelists (md_filename,
                             count_cb,
                             update_filelist_cb,
                             &info,
                             err);
    if (*err)
        goto cleanup;
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    update_info_remove_old_entries (update_info);
    yum_db_dbinfo_update (update_info->db, checksum, err);

 cleanup:
    update_info_done (update_info, err);

    if (info.pkg_handle)
        sqlite3_finalize (info.pkg_handle);
    if (info.file_handle)
        sqlite3_finalize (info.file_handle);
    if (update_info->db)
        sqlite3_close (update_info->db);

    if (*err) {
        g_free (db_filename);
        db_filename = NULL;
    }

    return db_filename;
}

/* Other */

typedef struct {
    UpdateInfo update_info;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *changelog_handle;
} UpdateOtherInfo;

static void
update_other_cb (Package *p, gpointer user_data)
{
    UpdateOtherInfo *info = (UpdateOtherInfo *) user_data;
    UpdateInfo *update_info = &info->update_info;

    g_hash_table_insert (update_info->all_packages,
                         g_string_chunk_insert (update_info->package_ids_chunk,
                                                p->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (update_info->current_packages,
                             p->pkgId) == NULL) {

        yum_db_package_ids_write (update_info->db, info->pkg_handle, p);
        yum_db_changelog_write (update_info->db, info->changelog_handle, p);
        update_info->add_count++;
    }

    if (update_info->count_from_md > 0 && update_info->progress_cb)
        update_info->progress_cb (++update_info->packages_seen,
                                  update_info->count_from_md,
                                  update_info->user_data);
}

static char *
update_other (const char *md_filename,
              const char *checksum,
              ProgressFn progress_cb,
              gpointer user_data,
              GError **err)
{
    UpdateOtherInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    memset (&info, 0, sizeof (UpdateOtherInfo));

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename, checksum,
                                   yum_db_create_other_tables,
                                   err);

    if (*err)
        goto cleanup;

    if (!update_info->db)
        return db_filename;

    update_info_init (update_info, err);
    if (*err)
        goto cleanup;
    update_info->progress_cb = progress_cb;
    update_info->user_data = user_data;
    info.pkg_handle = yum_db_package_ids_prepare (update_info->db, err);
    if (*err)
        goto cleanup;

    info.changelog_handle = yum_db_changelog_prepare (update_info->db, err);
    if (*err)
        goto cleanup;

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_other (md_filename,
                         count_cb,
                         update_other_cb,
                         &info,
                         err);
    if (*err)
        goto cleanup;
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    update_info_remove_old_entries (update_info);
    yum_db_dbinfo_update (update_info->db, checksum, err);

 cleanup:
    update_info_done (update_info, err);

    if (info.pkg_handle)
        sqlite3_finalize (info.pkg_handle);
    if (info.changelog_handle)
        sqlite3_finalize (info.changelog_handle);
    if (update_info->db)
        sqlite3_close (update_info->db);

    if (*err) {
        g_free (db_filename);
        db_filename = NULL;
    }

    return db_filename;
}

/*********************************************************************/

static gboolean
py_parse_args (PyObject *args,
               const char **md_filename,
               const char **checksum,
               PyObject **log,
               PyObject **progress)
{
    PyObject *callback;

    if (!PyArg_ParseTuple (args, "ssO", md_filename, checksum, &callback))
        return FALSE;

    if (PyObject_HasAttrString (callback, "log")) {
        *log = PyObject_GetAttrString (callback, "log");

        if (!PyCallable_Check (*log)) {
            PyErr_SetString (PyExc_TypeError, "parameter must be callable");
            return FALSE;
        }
    }

    if (PyObject_HasAttrString (callback, "progressbar")) {
        *progress = PyObject_GetAttrString (callback, "progressbar");

        if (!PyCallable_Check (*progress)) {
            PyErr_SetString (PyExc_TypeError, "parameter must be callable");
            return FALSE;
        }
    }

    return TRUE;
}

static void
progress_cb (guint32 current, guint32 total, gpointer user_data)
    {
    PyObject *progress = (PyObject *) user_data;
    PyObject *args;
    PyObject *result;

    args = PyTuple_New (2);
    PyTuple_SET_ITEM (args, 0, PyInt_FromLong (current));
    PyTuple_SET_ITEM (args, 1, PyInt_FromLong (total));

    result = PyEval_CallObject (progress, args);
    Py_DECREF (args);
    Py_XDECREF (result);
}

static void
log_cb (const gchar *log_domain,
        GLogLevelFlags log_level,
        const gchar *message,
        gpointer user_data)
{
    PyObject *callback = (PyObject *) user_data;
    int level;
    PyObject *args;
    PyObject *result;

    args = PyTuple_New (2);

    switch (log_level) {
    case G_LOG_LEVEL_DEBUG:
        level = 2;
        break;
    case G_LOG_LEVEL_MESSAGE:
        level = 1;
        break;
    case G_LOG_LEVEL_WARNING:
        level = 0;
        break;
    case G_LOG_LEVEL_CRITICAL:
    default:
        level = -1;
        break;
    }

    PyTuple_SET_ITEM (args, 0, PyInt_FromLong (level));
    PyTuple_SET_ITEM (args, 1, PyString_FromString (message));

    result = PyEval_CallObject (callback, args);
    Py_DECREF (args);
    Py_XDECREF (result);
}

typedef char * (*UpdateFn) (const char *md_filename,
                            const char *checksum,
                            ProgressFn progress_fn,
                            gpointer user_data,
                            GError **err);

static PyObject *
py_update (PyObject *self, PyObject *args, UpdateFn update_fn)
{
    const char *md_filename = NULL;
    const char *checksum = NULL;
    PyObject *log = NULL;
    PyObject *progress = NULL;
    guint log_id = 0;
    char *db_filename;
    PyObject *ret = NULL;
    GError *err = NULL;

    if (!py_parse_args (args, &md_filename, &checksum, &log, &progress))
        return NULL;

    if (log) {
        GLogLevelFlags level = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING |
            G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_DEBUG;
        log_id = g_log_set_handler (NULL, level, log_cb, log);
    }

    db_filename = update_fn (md_filename, checksum,
                             progress != NULL ? progress_cb : NULL,
                             progress, &err);

    if (log_id)
        g_log_remove_handler (NULL, log_id);

    if (db_filename) {
        ret = PyString_FromString (db_filename);
        g_free (db_filename);
    } else {
        PyErr_SetString (PyExc_TypeError, err->message);
        g_error_free (err);
    }

    return ret;
}


static PyObject *
py_update_primary (PyObject *self, PyObject *args)
{
    return py_update (self, args, update_primary);
}

static PyObject *
py_update_filelist (PyObject *self, PyObject *args)
{
    return py_update (self, args, update_filelist);
}

static PyObject *
py_update_other (PyObject *self, PyObject *args)
{
    return py_update (self, args, update_other);
}

static PyMethodDef SqliteMethods[] = {
    {"update_primary", py_update_primary, METH_VARARGS,
     "Parse YUM primary.xml metadata."},
    {"update_filelist", py_update_filelist, METH_VARARGS,
     "Parse YUM filelists.xml metadata."},
    {"update_other", py_update_other, METH_VARARGS,
     "Parse YUM other.xml metadata."},

    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
init_sqlitecache (void)
{
    Py_InitModule ("_sqlitecache", SqliteMethods);
}
