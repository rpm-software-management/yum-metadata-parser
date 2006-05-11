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

#ifndef __YUM_XML_PARSER_H__
#define __YUM_XML_PARSER_H__

#include "package.h"

void
yum_xml_parse_primary (const char *filename,
                       PackageFn callback,
                       gpointer user_data);

void
yum_xml_parse_filelists (const char *filename,
                         PackageFn callback,
                         gpointer user_data);

void yum_xml_parse_other (const char *filename,
                          PackageFn callback,
                          gpointer user_data);

#endif /* __YUM_XML_PARSER_H__ */
