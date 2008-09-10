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

typedef struct _UpdateInfo UpdateInfo;

typedef void (*InfoInitFn) (UpdateInfo *update_info, sqlite3 *db, GError **err);
typedef void (*InfoCleanFn) (UpdateInfo *update_info);

typedef void (*XmlParseFn)  (const char *filename,
                             CountFn count_callback,
                             PackageFn package_callback,
                             gpointer user_data,
                             GError **err);

typedef void (*WriteDbPackageFn) (UpdateInfo *update_info, Package *package);

typedef void (*IndexTablesFn) (sqlite3 *db, GError **err);

struct _UpdateInfo {
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
    gpointer python_callback;
    
    InfoInitFn info_init;
    InfoCleanFn info_clean;
    CreateTablesFn create_tables;
    WriteDbPackageFn write_package;
    XmlParseFn xml_parse;
    IndexTablesFn index_tables;

    gpointer user_data;
};

static void
update_info_init (UpdateInfo *info, GError **err)
{
    const char *sql;
    int rc;

    sql = "DELETE FROM packages WHERE pkgKey = ?";
    rc = sqlite3_prepare (info->db, sql, -1, &info->remove_handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare package removal: %s",
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
package_writer_info_init (UpdateInfo *update_info, sqlite3 *db, GError **err)
{
    PackageWriterInfo *info = (PackageWriterInfo *) update_info;

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
write_deps (sqlite3 *db, sqlite3_stmt *handle, gint64 pkgKey, 
            GSList *deps)
{
    GSList *iter;

    for (iter = deps; iter; iter = iter->next)
        yum_db_dependency_write (db, handle, pkgKey, (Dependency *) iter->data,
                                 FALSE);
}

static void
write_requirements (sqlite3 *db, sqlite3_stmt *handle, gint64 pkgKey,
            GSList *deps)
{
    GSList *iter;

    for (iter = deps; iter; iter = iter->next)
        yum_db_dependency_write (db, handle, pkgKey, (Dependency *) iter->data,
                                 TRUE);
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

    write_requirements (update_info->db, info->requires_handle,
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
package_writer_info_clean (UpdateInfo *update_info)
{
    PackageWriterInfo *info = (PackageWriterInfo *) update_info;
    
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


/* Filelists */

typedef struct {
    UpdateInfo update_info;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *file_handle;
} FileListInfo;

static void
update_filelist_info_init (UpdateInfo *update_info, sqlite3 *db, GError **err)
{
    FileListInfo *info = (FileListInfo *) update_info;

    info->pkg_handle = yum_db_package_ids_prepare (db, err);
    if (*err)
        return;

    info->file_handle = yum_db_filelists_prepare (db, err);
}

static void
update_filelist_info_clean (UpdateInfo *update_info)
{
    FileListInfo *info = (FileListInfo *) update_info;

    if (info->pkg_handle)
        sqlite3_finalize (info->pkg_handle);
    if (info->file_handle)
        sqlite3_finalize (info->file_handle);
}

static void
write_filelist_package_to_db (UpdateInfo *update_info, Package *package)
{
    FileListInfo *info = (FileListInfo *) update_info;

    yum_db_package_ids_write (update_info->db, info->pkg_handle, package);
    yum_db_filelists_write (update_info->db, info->file_handle, package);
}


/* Other */

typedef struct {
    UpdateInfo update_info;
    sqlite3_stmt *pkg_handle;
    sqlite3_stmt *changelog_handle;
} UpdateOtherInfo;

static void
update_other_info_init (UpdateInfo *update_info, sqlite3 *db, GError **err)
{
    UpdateOtherInfo *info = (UpdateOtherInfo *) update_info;
    info->pkg_handle = yum_db_package_ids_prepare (db, err);
    if (*err)
        return;

    info->changelog_handle = yum_db_changelog_prepare (db, err);
}

static void
update_other_info_clean (UpdateInfo *update_info)
{
    UpdateOtherInfo *info = (UpdateOtherInfo *) update_info;

    if (info->pkg_handle)
        sqlite3_finalize (info->pkg_handle);
    if (info->changelog_handle)
        sqlite3_finalize (info->changelog_handle);
}

static void
write_other_package_to_db (UpdateInfo *update_info, Package *package)
{
    UpdateOtherInfo *info = (UpdateOtherInfo *) update_info;

    yum_db_package_ids_write (update_info->db, info->pkg_handle, package);
    yum_db_changelog_write (update_info->db, info->changelog_handle, package);
}


/*****************************************************************************/

static void
progress_cb (UpdateInfo *update_info)
{
    PyObject *progress = (PyObject *) update_info->python_callback;
    PyObject *repoid = (PyObject *) update_info->user_data;
    PyObject *args;
    PyObject *result;

    Py_INCREF(repoid);
   
    args = PyTuple_New (3);
    PyTuple_SET_ITEM (args, 0, PyInt_FromLong (update_info->packages_seen));
    PyTuple_SET_ITEM (args, 1, PyInt_FromLong (update_info->count_from_md));
    PyTuple_SET_ITEM (args, 2, repoid);

    result = PyEval_CallObject (progress, args);
    Py_DECREF (args);
    Py_XDECREF (result);
}

static void
update_package_cb (Package *p, gpointer user_data)
{
    UpdateInfo *update_info = (UpdateInfo *) user_data;

    /* TODO: Wire in logging of skipped packages */
    if (p->pkgId == NULL) {
        return;
    }

    g_hash_table_insert (update_info->all_packages,
                         g_string_chunk_insert (update_info->package_ids_chunk,
                                                p->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (update_info->current_packages,
                             p->pkgId) == NULL) {
        
        update_info->write_package (update_info, p);
        update_info->add_count++;
    }

    if (update_info->count_from_md > 0 && update_info->python_callback) {
        update_info->packages_seen++;
        progress_cb (update_info);
    }
}

static char *
update_packages (UpdateInfo *update_info,
                 const char *md_filename,
                 const char *checksum,
                 gpointer python_callback,
                 gpointer user_data,
                 GError **err)
{
    char *db_filename;

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename, checksum,
                                   update_info->create_tables,
                                   err);

    if (*err)
        goto cleanup;

    if (!update_info->db)
        return db_filename;

    update_info_init (update_info, err);
    if (*err)
        goto cleanup;
    
    update_info->python_callback = python_callback;
    update_info->user_data = user_data;

    update_info->info_init (update_info, update_info->db, err);
    if (*err)
        goto cleanup;

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    update_info->xml_parse (md_filename,
                            count_cb,
                            update_package_cb,
                            update_info,
                            err);
    if (*err)
        goto cleanup;
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    update_info->index_tables (update_info->db, err);
    if (*err)
        goto cleanup;

    update_info_remove_old_entries (update_info);
    yum_db_dbinfo_update (update_info->db, checksum, err);

 cleanup:
    update_info->info_clean (update_info);
    update_info_done (update_info, err);

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
               PyObject **progress,
               PyObject **repoid)
{
    PyObject *callback;

    if (!PyArg_ParseTuple (args, "ssOO", md_filename, checksum, &callback,
                           repoid))
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
log_cb (const gchar *log_domain,
        GLogLevelFlags log_level,
        const gchar *message,
        gpointer user_data)
{
    PyObject *callback = (PyObject *) user_data;
    int level;
    PyObject *args;
    PyObject *result;

    if (!callback)
        return;

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

static PyObject *
py_update (PyObject *self, PyObject *args, UpdateInfo *update_info)
{
    const char *md_filename = NULL;
    const char *checksum = NULL;
    PyObject *log = NULL;
    PyObject *progress = NULL;
    PyObject *repoid = NULL;
    guint log_id = 0;
    char *db_filename;
    PyObject *ret = NULL;
    GError *err = NULL;

    if (!py_parse_args (args, &md_filename, &checksum, &log, &progress,
                        &repoid))
        return NULL;

    GLogLevelFlags level = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING |
        G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_DEBUG;
    log_id = g_log_set_handler (NULL, level, log_cb, log);

    db_filename = update_packages (update_info, md_filename, checksum,
                                   progress, repoid, &err);

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
    PackageWriterInfo info;
    memset (&info, 0, sizeof (PackageWriterInfo));

    info.update_info.info_init = package_writer_info_init;
    info.update_info.info_clean = package_writer_info_clean;
    info.update_info.create_tables = yum_db_create_primary_tables;
    info.update_info.write_package = write_package_to_db;
    info.update_info.xml_parse = yum_xml_parse_primary;
    info.update_info.index_tables = yum_db_index_primary_tables;

    return py_update (self, args, (UpdateInfo *) &info);
}

static PyObject *
py_update_filelist (PyObject *self, PyObject *args)
{
    FileListInfo info;
    memset (&info, 0, sizeof (FileListInfo));

    info.update_info.info_init = update_filelist_info_init;
    info.update_info.info_clean = update_filelist_info_clean;
    info.update_info.create_tables = yum_db_create_filelist_tables;
    info.update_info.write_package = write_filelist_package_to_db;
    info.update_info.xml_parse = yum_xml_parse_filelists;
    info.update_info.index_tables = yum_db_index_filelist_tables;

    return py_update (self, args, (UpdateInfo *) &info);
}

static PyObject *
py_update_other (PyObject *self, PyObject *args)
{
    UpdateOtherInfo info;
    memset (&info, 0, sizeof (UpdateOtherInfo));

    info.update_info.info_init = update_other_info_init;
    info.update_info.info_clean = update_other_info_clean;
    info.update_info.create_tables = yum_db_create_other_tables;
    info.update_info.write_package = write_other_package_to_db;
    info.update_info.xml_parse = yum_xml_parse_other;
    info.update_info.index_tables = yum_db_index_other_tables;

    return py_update (self, args, (UpdateInfo *) &info);
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
    PyObject * m, * d;

    m = Py_InitModule ("_sqlitecache", SqliteMethods);

    d = PyModule_GetDict(m);
    PyDict_SetItemString(d, "DBVERSION", PyInt_FromLong(YUM_SQLITE_CACHE_DBVERSION));
}
