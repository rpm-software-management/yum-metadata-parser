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

#include "debug.h"
#include "xml-parser.h"
#include "db.h"
#include "package.h"


typedef struct {
    sqlite3 *db;
    sqlite3_stmt *remove_handle;
    int add_count;
    int del_count;
    GHashTable *current_packages;
    GHashTable *all_packages;
    GTimer *timer;
} UpdateInfo;

static void
update_info_init (UpdateInfo *info)
{
    const char *sql;
    int rc;

    sql = "DELETE FROM packages WHERE pkgKey = ?";
    rc = sqlite3_prepare (info->db, sql, -1, &info->remove_handle, NULL);
    if (rc != SQLITE_OK) {
        debug (DEBUG_LEVEL_WARNING,
               "Can not prepare package removal clause: %s",
               sqlite3_errmsg (info->db));
        return;
    }

    info->add_count = 0;
    info->del_count = 0;
    info->current_packages = yum_db_read_package_ids (info->db);
    info->all_packages = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                (GDestroyNotify) g_free, NULL);
    info->timer = g_timer_new ();
    g_timer_start (info->timer);
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
            debug (DEBUG_LEVEL_WARNING,
                   "Error removing package from SQL: %s",
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
update_info_done (UpdateInfo *info)
{
    sqlite3_finalize (info->remove_handle);
    g_hash_table_destroy (info->current_packages);
    g_hash_table_destroy (info->all_packages);

    g_timer_stop (info->timer);
    debug (DEBUG_LEVEL_INFO,
           "Added %d new packages, deleted %d old in %.2f seconds",
           info->add_count, info->del_count,
           g_timer_elapsed (info->timer, NULL));
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
package_writer_info_init (PackageWriterInfo *info, sqlite3 *db)
{
    info->pkg_handle = yum_db_package_prepare (db);
    info->requires_handle = yum_db_dependency_prepare (db, "requires");
    info->provides_handle = yum_db_dependency_prepare (db, "provides");
    info->conflicts_handle = yum_db_dependency_prepare (db, "conflicts");
    info->obsoletes_handle = yum_db_dependency_prepare (db, "obsoletes");
    info->files_handle = yum_db_file_prepare (db);
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
                         g_strdup (package->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (info->current_packages,
                             package->pkgId) == NULL) {

        write_package_to_db (info, package);
        info->add_count++;
    }
}

static void
package_writer_info_clean (PackageWriterInfo *info)
{
    sqlite3_finalize (info->pkg_handle);
    sqlite3_finalize (info->requires_handle);
    sqlite3_finalize (info->provides_handle);
    sqlite3_finalize (info->conflicts_handle);
    sqlite3_finalize (info->obsoletes_handle);
    sqlite3_finalize (info->files_handle);
}

static char *
update_primary (const char *md_filename, const char *checksum)
{
    PackageWriterInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename);

    if (!update_info->db) {
        g_free (db_filename);
        return NULL;
    }

    if (yum_db_dbinfo_fresh (update_info->db, checksum)) {
        sqlite3_close (update_info->db);
        return db_filename;
    }

    yum_db_create_primary_tables (update_info->db);
    yum_db_dbinfo_clear (update_info->db);

    update_info_init (update_info);
    package_writer_info_init (&info, update_info->db);

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_primary (md_filename, add_package, &info);
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    package_writer_info_clean (&info);

    update_info_remove_old_entries (update_info);
    update_info_done (update_info);

    yum_db_dbinfo_update (update_info->db, checksum);

    sqlite3_close (update_info->db);

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
                         g_strdup (p->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (update_info->current_packages,
                             p->pkgId) == NULL) {

        yum_db_package_ids_write (update_info->db, info->pkg_handle, p);
        yum_db_filelists_write (update_info->db, info->file_handle, p);
        update_info->add_count++;
    }
}

static char *
update_filelist (const char *md_filename, const char *checksum)
{
    FileListInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename);

    if (!update_info->db) {
        g_free (db_filename);
        return NULL;
    }

    if (yum_db_dbinfo_fresh (update_info->db, checksum)) {
        sqlite3_close (update_info->db);
        return db_filename;
    }

    yum_db_create_filelist_tables (update_info->db);
    yum_db_dbinfo_clear (update_info->db);

    update_info_init (update_info);
    info.pkg_handle = yum_db_package_ids_prepare (update_info->db);
    info.file_handle = yum_db_filelists_prepare (update_info->db);

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_filelists (md_filename, update_filelist_cb, &info);
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize (info.pkg_handle);
    sqlite3_finalize (info.file_handle);

    update_info_remove_old_entries (update_info);
    update_info_done (update_info);

    yum_db_dbinfo_update (update_info->db, checksum);

    sqlite3_close (update_info->db);

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
                         g_strdup (p->pkgId),
                         GINT_TO_POINTER (1));

    if (g_hash_table_lookup (update_info->current_packages,
                             p->pkgId) == NULL) {

        yum_db_package_ids_write (update_info->db, info->pkg_handle, p);
        yum_db_changelog_write (update_info->db, info->changelog_handle, p);
        update_info->add_count++;
    }
}

static char *
update_other (const char *md_filename, const char *checksum)
{
    UpdateOtherInfo info;
    UpdateInfo *update_info = &info.update_info;
    char *db_filename;

    db_filename = yum_db_filename (md_filename);
    update_info->db = yum_db_open (db_filename);

    if (!update_info->db) {
        g_free (db_filename);
        return NULL;
    }

    if (yum_db_dbinfo_fresh (update_info->db, checksum)) {
        sqlite3_close (update_info->db);
        return db_filename;
    }

    yum_db_create_other_tables (update_info->db);
    yum_db_dbinfo_clear (update_info->db);

    update_info_init (update_info);
    info.pkg_handle = yum_db_package_ids_prepare (update_info->db);
    info.changelog_handle = yum_db_changelog_prepare (update_info->db);

    sqlite3_exec (update_info->db, "BEGIN", NULL, NULL, NULL);
    yum_xml_parse_other (md_filename, update_other_cb, &info);
    sqlite3_exec (update_info->db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize (info.pkg_handle);
    sqlite3_finalize (info.changelog_handle);

    update_info_remove_old_entries (update_info);
    update_info_done (update_info);

    yum_db_dbinfo_update (update_info->db, checksum);

    sqlite3_close (update_info->db);

    return db_filename;
}

/*********************************************************************/

static void
debug_cb (const char *message,
          DebugLevel level,
          gpointer user_data)
{
    PyObject *callback = (PyObject *) user_data;
    PyObject *arglist;
    PyObject *result;

    arglist = Py_BuildValue ("(is)", level, message);
    result = PyEval_CallObject (callback, arglist);
    Py_DECREF (arglist);

    if (result) {
        Py_DECREF (result);
    }
}

static PyObject *
py_log_handler_add (PyObject *self, PyObject *args)
{
    PyObject *callback;
    int id;

    if (!PyArg_ParseTuple (args, "O", &callback))
        return NULL;

    if (!PyCallable_Check (callback)) {
        PyErr_SetString (PyExc_TypeError, "parameter must be callable");
        return NULL;
    }

    Py_XINCREF (callback);
    id = debug_add_handler (debug_cb, callback);

    return Py_BuildValue ("i", id);
}

static PyObject *
py_log_handler_remove (PyObject *self, PyObject *args)
{
    int id;

    if (!PyArg_ParseTuple (args, "i", &id))
        return NULL;

    debug_remove_handler (id);

    Py_INCREF (Py_None);
    return Py_None;
}

static PyObject *
py_update_primary (PyObject *self, PyObject *args)
{
    const char *md_filename;
    const char *checksum;
    char *db_filename;
    PyObject *ret;

    if (!PyArg_ParseTuple (args, "ss", &md_filename, &checksum))
        return NULL;

    db_filename = update_primary (md_filename, checksum);
    if (db_filename) {
        ret = Py_BuildValue ("s", db_filename);
        g_free (db_filename);
    } else {
        Py_INCREF (Py_None);
        ret = Py_None;
    }

    return ret;
}

static PyObject *
py_update_filelist (PyObject *self, PyObject *args)
{
    const char *md_filename;
    const char *checksum;
    char *db_filename;
    PyObject *ret;

    if (!PyArg_ParseTuple (args, "ss", &md_filename, &checksum))
        return NULL;

    db_filename = update_filelist (md_filename, checksum);
    if (db_filename) {
        ret = Py_BuildValue ("s", db_filename);
        g_free (db_filename);
    } else {
        Py_INCREF (Py_None);
        ret = Py_None;
    }

    return ret;
}

static PyObject *
py_update_other (PyObject *self, PyObject *args)
{
    const char *md_filename;
    const char *checksum;
    char *db_filename;
    PyObject *ret;

    if (!PyArg_ParseTuple (args, "ss", &md_filename, &checksum))
        return NULL;

    db_filename = update_other (md_filename, checksum);
    if (db_filename) {
        ret = Py_BuildValue ("s", db_filename);
        g_free (db_filename);
    } else {
        Py_INCREF (Py_None);
        ret = Py_None;
    }

    return ret;
}

static PyMethodDef SqliteMethods[] = {
    {"log_handler_add", py_log_handler_add, METH_VARARGS,
     "Add log handler."},
    {"log_handler_remove", py_log_handler_remove, METH_VARARGS,
     "Remove log handler."},
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
