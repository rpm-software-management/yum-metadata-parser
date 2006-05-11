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

#ifndef __YUM_DB_H__
#define __YUM_DB_H__

#include <glib.h>
#include <sqlite3.h>
#include "package.h"

#define YUM_SQLITE_CACHE_DBVERSION 8

char         *yum_db_filename               (const char *prefix);
sqlite3      *yum_db_open                   (const char *path);

void          yum_db_dbinfo_clear           (sqlite3 *db);
gboolean      yum_db_dbinfo_fresh           (sqlite3 *db,
                                             const char *checksum);
void          yum_db_dbinfo_update          (sqlite3 *db,
                                             const char *checksum);

GHashTable   *yum_db_read_package_ids       (sqlite3 *db);

/* Primary */

void          yum_db_create_primary_tables  (sqlite3 *db);
sqlite3_stmt *yum_db_package_prepare        (sqlite3 *db);
void          yum_db_package_write          (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             Package *p);

sqlite3_stmt *yum_db_dependency_prepare     (sqlite3 *db, const char *table);
void          yum_db_dependency_write       (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             gint64 pkgKey,
                                             Dependency *dep);

sqlite3_stmt *yum_db_file_prepare           (sqlite3 *db);
void          yum_db_file_write             (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             gint64 pkgKey,
                                             PackageFile *file);

/* Filelists */

void          yum_db_create_filelist_tables (sqlite3 *db);
sqlite3_stmt *yum_db_package_ids_prepare    (sqlite3 *db);
void          yum_db_package_ids_write      (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             Package *p);

sqlite3_stmt *yum_db_filelists_prepare      (sqlite3 *db);
void          yum_db_filelists_write        (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             Package *p);

/* Other */
void          yum_db_create_other_tables    (sqlite3 *db);
sqlite3_stmt *yum_db_changelog_prepare      (sqlite3 *db);
void          yum_db_changelog_write        (sqlite3 *db,
                                             sqlite3_stmt *handle,
                                             Package *p);


#endif /* __YUM_DB_H__ */
