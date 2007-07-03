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

#include <string.h>
#include <glib.h>
#include <sqlite3.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "xml-parser.h"

#define PACKAGE_FIELD_SIZE 1024

GQuark
yum_parser_error_quark (void)
{
    static GQuark quark;

    if (!quark)
        quark = g_quark_from_static_string ("yum_parser_error");

    return quark;
}

static guint32
string_to_guint32_with_default (const char *n, guint32 def)
{
    char *ret;
    guint32 z;

    z = strtoul (n, &ret, 10);
    if (*ret != '\0')
        return def;
    else
        return z;
}

typedef struct {
    const char *md_type;
    xmlParserCtxt *xml_context;
    GError **error;
    CountFn count_fn;
    PackageFn package_fn;
    gpointer user_data;

    Package *current_package;

    gboolean want_text;
    GString *text_buffer;
} SAXContext;

typedef enum {
    PRIMARY_PARSER_TOPLEVEL = 0,
    PRIMARY_PARSER_PACKAGE,
    PRIMARY_PARSER_FORMAT,
    PRIMARY_PARSER_DEP,
} PrimarySAXContextState;

typedef struct {
    SAXContext sctx;

    PrimarySAXContextState state;

    GSList **current_dep_list;
    PackageFile *current_file;
} PrimarySAXContext;

static void
primary_parser_toplevel_start (PrimarySAXContext *ctx,
                               const char *name,
                               const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    if (!strcmp (name, "package")) {
        g_assert (sctx->current_package == NULL);

        ctx->state = PRIMARY_PARSER_PACKAGE;

        sctx->current_package = package_new ();
    }

    else if (sctx->count_fn && !strcmp (name, "metadata")) {
        int i;
        const char *attr;
        const char *value;

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "packages")) {
                sctx->count_fn (string_to_guint32_with_default (value, 0),
                               sctx->user_data);
                break;
            }
        }
    }
}

static void
parse_version_info(const char **attrs, Package *p)
{
    int i;
    const char *attr;
    const char *value;

    for (i = 0; attrs && attrs[i]; i++) {
        attr = attrs[i];
        value = attrs[++i];

        if (!strcmp (attr, "epoch"))
            p->epoch = g_string_chunk_insert (p->chunk, value);
        else if (!strcmp (attr, "ver"))
            p->version = g_string_chunk_insert (p->chunk, value);
        else if (!strcmp (attr, "rel"))
            p->release = g_string_chunk_insert (p->chunk, value);
    }
}

static void
primary_parser_package_start (PrimarySAXContext *ctx,
                              const char *name,
                              const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    sctx->want_text = TRUE;

    if (!strcmp (name, "format")) {
        ctx->state = PRIMARY_PARSER_FORMAT;
    }

    else if (!strcmp (name, "version")) {
        parse_version_info(attrs, p);
    }

    else if (!strcmp (name, "checksum")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type"))
                p->checksum_type = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "time")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "file"))
                p->time_file = strtol(value, NULL, 10);
            else if (!strcmp (attr, "build"))
                p->time_build = strtol(value, NULL, 10);
        }
    }

    else if (!strcmp (name, "size")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "package"))
                p->size_package = strtol(value, NULL, 10);
            else if (!strcmp (attr, "installed"))
                p->size_installed = strtol(value, NULL, 10);
            else if (!strcmp (attr, "archive"))
                p->size_archive = strtol(value, NULL, 10);
        }
    }

    else if (!strcmp (name, "location")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "href"))
                p->location_href = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "xml:base"))
                p->location_base = g_string_chunk_insert (p->chunk, value);
        }
    }
}

static void
primary_parser_format_start (PrimarySAXContext *ctx,
                             const char *name,
                             const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    if (!strcmp (name, "rpm:header-range")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "start"))
                p->rpm_header_start = strtol(value, NULL, 10);
            else if (!strcmp (attr, "end"))
                p->rpm_header_end = strtol(value, NULL, 10);
        }
    }

    else if (!strcmp (name, "rpm:provides")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &sctx->current_package->provides;
    } else if (!strcmp (name, "rpm:requires")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &sctx->current_package->requires;
    } else if (!strcmp (name, "rpm:obsoletes")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &sctx->current_package->obsoletes;
    } else if (!strcmp (name, "rpm:conflicts")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &sctx->current_package->conflicts;
    }

    else if (!strcmp (name, "file")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type")) {
                ctx->current_file = package_file_new ();
                ctx->current_file->type =
                    g_string_chunk_insert_const (p->chunk, value);
            }
        }
    }
}

static void
primary_parser_dep_start (PrimarySAXContext *ctx,
                          const char *name,
                          const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    const char *tmp_name = NULL;
    const char *tmp_version = NULL;
    const char *tmp_release = NULL;
    const char *tmp_epoch = NULL;
    const char *tmp_flags = NULL;
    gboolean tmp_pre = FALSE;
    Dependency *dep;
    int i;
    gboolean ignore = FALSE;
    const char *attr;
    const char *value;

    if (!strcmp (name, "rpm:entry")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "name")) {
                if (!strncmp (value, "rpmlib(", strlen ("rpmlib("))) {
                    ignore = TRUE;
                    break;
                }
                tmp_name = value;
            } else if (!strcmp (attr, "flags"))
                tmp_flags = value;
            else if (!strcmp (attr, "epoch"))
                tmp_epoch = value;
            else if (!strcmp (attr, "ver"))
                tmp_version = value;
            else if (!strcmp (attr, "rel"))
                tmp_release = value;
            else if (!strcmp (attr, "pre"))
                tmp_pre = TRUE;
        }

        if (!ignore) {
            GStringChunk *chunk = sctx->current_package->chunk;

            dep = dependency_new ();
            dep->name = g_string_chunk_insert (chunk, tmp_name);
            if (tmp_flags)
                dep->flags = g_string_chunk_insert (chunk, tmp_flags);
            if (tmp_epoch)
                dep->epoch = g_string_chunk_insert (chunk, tmp_epoch);
            if (tmp_version)
                dep->version = g_string_chunk_insert (chunk, tmp_version);
            if (tmp_release)
                dep->release = g_string_chunk_insert (chunk, tmp_release);
            dep->pre = tmp_pre;

            *ctx->current_dep_list = g_slist_prepend (*ctx->current_dep_list,
                                                      dep);
        }
    }
}

static void
primary_sax_start_element (void *data, const char *name, const char **attrs)
{
    PrimarySAXContext *ctx = (PrimarySAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    if (sctx->text_buffer->len)
        g_string_truncate (sctx->text_buffer, 0);

    switch (ctx->state) {
    case PRIMARY_PARSER_TOPLEVEL:
        primary_parser_toplevel_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_PACKAGE:
        primary_parser_package_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_FORMAT:
        primary_parser_format_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_DEP:
        primary_parser_dep_start (ctx, name, attrs);
        break;

    default:
        break;
    }
}

static void
primary_parser_package_end (PrimarySAXContext *ctx, const char *name)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "package")) {
        if (sctx->package_fn && !*sctx->error)
            sctx->package_fn (p, sctx->user_data);

        package_free (p);
        sctx->current_package = NULL;

        sctx->want_text = FALSE;
        ctx->state = PRIMARY_PARSER_TOPLEVEL;
    }

    else if (sctx->text_buffer->len == 0)
        /* Nothing interesting to do here */
        return;

    else if (!strcmp (name, "name"))
        p->name = g_string_chunk_insert_len (p->chunk,
                                             sctx->text_buffer->str,
                                             sctx->text_buffer->len);
    else if (!strcmp (name, "arch"))
        p->arch = g_string_chunk_insert_len (p->chunk,
                                             sctx->text_buffer->str,
                                             sctx->text_buffer->len);
    else if (!strcmp (name, "checksum"))
        p->pkgId = g_string_chunk_insert_len (p->chunk,
                                              sctx->text_buffer->str,
                                              sctx->text_buffer->len);
    else if (!strcmp (name, "summary"))
        p->summary = g_string_chunk_insert_len (p->chunk,
                                                sctx->text_buffer->str,
                                                sctx->text_buffer->len);
    else if (!strcmp (name, "description"))
        p->description = g_string_chunk_insert_len (p->chunk,
                                                    sctx->text_buffer->str,
                                                    sctx->text_buffer->len);
    else if (!strcmp (name, "packager"))
        p->rpm_packager = g_string_chunk_insert_len (p->chunk,
                                                     sctx->text_buffer->str,
                                                     sctx->text_buffer->len);
    else if (!strcmp (name, "url"))
        p->url = g_string_chunk_insert_len (p->chunk,
                                            sctx->text_buffer->str,
                                            sctx->text_buffer->len);
}

static void
primary_parser_format_end (PrimarySAXContext *ctx, const char *name)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "rpm:license"))
        p->rpm_license = g_string_chunk_insert_len (p->chunk,
                                                    sctx->text_buffer->str,
                                                    sctx->text_buffer->len);
    if (!strcmp (name, "rpm:vendor"))
        p->rpm_vendor = g_string_chunk_insert_len (p->chunk,
                                                   sctx->text_buffer->str,
                                                   sctx->text_buffer->len);
    if (!strcmp (name, "rpm:group"))
        p->rpm_group = g_string_chunk_insert_len (p->chunk,
                                                  sctx->text_buffer->str,
                                                  sctx->text_buffer->len);
    if (!strcmp (name, "rpm:buildhost"))
        p->rpm_buildhost = g_string_chunk_insert_len (p->chunk,
                                                      sctx->text_buffer->str,
                                                      sctx->text_buffer->len);
    if (!strcmp (name, "rpm:sourcerpm"))
        p->rpm_sourcerpm = g_string_chunk_insert_len (p->chunk,
                                                      sctx->text_buffer->str,
                                                      sctx->text_buffer->len);
    else if (!strcmp (name, "file")) {
        PackageFile *file = ctx->current_file != NULL ?
            ctx->current_file : package_file_new ();

        file->name = g_string_chunk_insert_len (p->chunk,
                                                sctx->text_buffer->str,
                                                sctx->text_buffer->len);

        if (!file->type)
            file->type = g_string_chunk_insert_const (p->chunk, "file");

        p->files = g_slist_prepend (p->files, file);
        ctx->current_file = NULL;
    } else if (!strcmp (name, "format"))
        ctx->state = PRIMARY_PARSER_PACKAGE;
}

static void
primary_parser_dep_end (PrimarySAXContext *ctx, const char *name)
{
    SAXContext *sctx = &ctx->sctx;

    g_assert (sctx->current_package != NULL);

    if (strcmp (name, "rpm:entry"))
        ctx->state = PRIMARY_PARSER_FORMAT;
}

static void
primary_sax_end_element (void *data, const char *name)
{
    PrimarySAXContext *ctx = (PrimarySAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    switch (ctx->state) {
    case PRIMARY_PARSER_PACKAGE:
        primary_parser_package_end (ctx, name);
        break;
    case PRIMARY_PARSER_FORMAT:
        primary_parser_format_end (ctx, name);
        break;
    case PRIMARY_PARSER_DEP:
        primary_parser_dep_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (sctx->text_buffer, 0);
}

static void
sax_characters (void *data, const char *ch, int len)
{
    SAXContext *sctx = (SAXContext *) data;

    if (sctx->want_text)
        g_string_append_len (sctx->text_buffer, ch, len);
}

static void
sax_warning (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    g_warning ("* SAX Warning: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static void
sax_error (void *data, const char *msg, ...)
{
    SAXContext *sctx = (SAXContext *) data;
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    g_set_error (sctx->error, YUM_PARSER_ERROR, YUM_PARSER_ERROR,
                 "Parsing %s error: %s", sctx->md_type, tmp);
    g_free (tmp);

    va_end (args);
}

static xmlSAXHandler primary_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) primary_sax_start_element, /* startElement */
    (endElementSAXFunc) primary_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    sax_warning,      /* warning */
    sax_error,      /* error */
    sax_error,      /* fatalError */
};

void
sax_context_init (SAXContext *sctx,
                  const char *md_type,
                  CountFn count_callback,
                  PackageFn package_callback,
                  gpointer user_data,
                  GError **err)
{
    sctx->md_type = md_type;
    sctx->error = err;
    sctx->count_fn = count_callback;
    sctx->package_fn = package_callback;
    sctx->user_data = user_data;
    sctx->current_package = NULL;
    sctx->want_text = FALSE;
    sctx->text_buffer = g_string_sized_new (PACKAGE_FIELD_SIZE);
}

void
yum_xml_parse_primary (const char *filename,
                       CountFn count_callback,
                       PackageFn package_callback,
                       gpointer user_data,
                       GError **err)
{
    PrimarySAXContext ctx;
    SAXContext *sctx = &ctx.sctx;
    int rc;

    ctx.state = PRIMARY_PARSER_TOPLEVEL;
    ctx.current_dep_list = NULL;
    ctx.current_file = NULL;

    sax_context_init(sctx, "primary.xml", count_callback, package_callback,
                     user_data, err);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&primary_sax_handler, &ctx, filename);

    if (sctx->current_package) {
        g_warning ("Incomplete package lost");
        package_free (sctx->current_package);
    }

    g_string_free (sctx->text_buffer, TRUE);
}

/*****************************************************************************/


static void
parse_package (const char **attrs, Package *p)
{
    int i;
    const char *attr;
    const char *value;

    for (i = 0; attrs && attrs[i]; i++) {
        attr = attrs[i];
        value = attrs[++i];

        if (!strcmp (attr, "pkgid"))
            p->pkgId = g_string_chunk_insert (p->chunk, value);
        if (!strcmp (attr, "name"))
            p->name = g_string_chunk_insert (p->chunk, value);
        else if (!strcmp (attr, "arch"))
            p->arch = g_string_chunk_insert (p->chunk, value);
    }
}

typedef enum {
    FILELIST_PARSER_TOPLEVEL = 0,
    FILELIST_PARSER_PACKAGE,
} FilelistSAXContextState;

typedef struct {
    SAXContext sctx;

    FilelistSAXContextState state;

    PackageFile *current_file;
} FilelistSAXContext;

static void
filelist_parser_toplevel_start (FilelistSAXContext *ctx,
                                const char *name,
                                const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    if (!strcmp (name, "package")) {
        g_assert (sctx->current_package == NULL);

        ctx->state = FILELIST_PARSER_PACKAGE;

        sctx->current_package = package_new ();
        parse_package (attrs, sctx->current_package);
    }

    else if (sctx->count_fn && !strcmp (name, "filelists")) {
        int i;
        const char *attr;
        const char *value;

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "packages")) {
                sctx->count_fn (string_to_guint32_with_default (value, 0),
                                sctx->user_data);
                break;
            }
        }
    }
}

static void
filelist_parser_package_start (FilelistSAXContext *ctx,
                               const char *name,
                               const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    sctx->want_text = TRUE;

    if (!strcmp (name, "version")) {
        parse_version_info(attrs, p);
    }

    else if (!strcmp (name, "file")) {
        ctx->current_file = package_file_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type"))
                ctx->current_file->type =
                    g_string_chunk_insert_const (p->chunk, value);
        }
    }
}

static void
filelist_sax_start_element (void *data, const char *name, const char **attrs)
{
    FilelistSAXContext *ctx = (FilelistSAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    if (sctx->text_buffer->len)
        g_string_truncate (sctx->text_buffer, 0);

    switch (ctx->state) {
    case FILELIST_PARSER_TOPLEVEL:
        filelist_parser_toplevel_start (ctx, name, attrs);
        break;
    case FILELIST_PARSER_PACKAGE:
        filelist_parser_package_start (ctx, name, attrs);
        break;
    default:
        break;
    }
}

static void
filelist_parser_package_end (FilelistSAXContext *ctx, const char *name)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;

    g_assert (p != NULL);

    sctx->want_text = FALSE;

    if (!strcmp (name, "package")) {
        if (sctx->package_fn && !*sctx->error)
            sctx->package_fn (p, sctx->user_data);

        package_free (p);
        sctx->current_package = NULL;

        if (ctx->current_file) {
            g_free (ctx->current_file);
            ctx->current_file = NULL;
        }

        ctx->state = FILELIST_PARSER_TOPLEVEL;
    }

    else if (!strcmp (name, "file")) {
        PackageFile *file = ctx->current_file;
        file->name = g_string_chunk_insert_len (p->chunk,
                                                sctx->text_buffer->str,
                                                sctx->text_buffer->len);
        if (!file->type)
            file->type = g_string_chunk_insert_const (p->chunk, "file");

        p->files = g_slist_prepend (p->files, file);
        ctx->current_file = NULL;
    }
}

static void
filelist_sax_end_element (void *data, const char *name)
{
    FilelistSAXContext *ctx = (FilelistSAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    switch (ctx->state) {
    case FILELIST_PARSER_PACKAGE:
        filelist_parser_package_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (sctx->text_buffer, 0);
}

static xmlSAXHandler filelist_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) filelist_sax_start_element, /* startElement */
    (endElementSAXFunc) filelist_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    sax_warning,      /* warning */
    sax_error,      /* error */
    sax_error,      /* fatalError */
};

void
yum_xml_parse_filelists (const char *filename,
                         CountFn count_callback,
                         PackageFn package_callback,
                         gpointer user_data,
                         GError **err)
{
    FilelistSAXContext ctx;
    SAXContext *sctx = &ctx.sctx;

    int rc;

    ctx.state = FILELIST_PARSER_TOPLEVEL;
    ctx.current_file = NULL;
    
    sax_context_init(sctx, "filelists.xml", count_callback, package_callback,
                     user_data, err);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&filelist_sax_handler, &ctx, filename);

    if (sctx->current_package) {
        g_warning ("Incomplete package lost");
        package_free (sctx->current_package);
    }

    if (ctx.current_file)
        g_free (ctx.current_file);

    g_string_free (sctx->text_buffer, TRUE);
}

/*****************************************************************************/

typedef enum {
    OTHER_PARSER_TOPLEVEL = 0,
    OTHER_PARSER_PACKAGE,
} OtherSAXContextState;

typedef struct {
    SAXContext sctx;

    OtherSAXContextState state;

    ChangelogEntry *current_entry;
} OtherSAXContext;

static void
other_parser_toplevel_start (OtherSAXContext *ctx,
                             const char *name,
                             const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    if (!strcmp (name, "package")) {
        g_assert (sctx->current_package == NULL);

        ctx->state = OTHER_PARSER_PACKAGE;

        sctx->current_package = package_new ();
        parse_package (attrs, sctx->current_package);
    }

    else if (sctx->count_fn && !strcmp (name, "otherdata")) {
        int i;
        const char *attr;
        const char *value;

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "packages")) {
                sctx->count_fn (string_to_guint32_with_default (value, 0),
                                sctx->user_data);
                break;
            }
        }
    }
}

static void
other_parser_package_start (OtherSAXContext *ctx,
                            const char *name,
                            const char **attrs)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    sctx->want_text = TRUE;

    if (!strcmp (name, "version")) {
        parse_version_info(attrs, p);
    }

    else if (!strcmp (name, "changelog")) {
        ctx->current_entry = changelog_entry_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "author"))
                ctx->current_entry->author =
                    g_string_chunk_insert_const (p->chunk, value);
            else if (!strcmp (attr, "date"))
                ctx->current_entry->date = strtol(value, NULL, 10);
        }
    }
}

static void
other_sax_start_element (void *data, const char *name, const char **attrs)
{
    OtherSAXContext *ctx = (OtherSAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    if (sctx->text_buffer->len)
        g_string_truncate (sctx->text_buffer, 0);

    switch (ctx->state) {
    case OTHER_PARSER_TOPLEVEL:
        other_parser_toplevel_start (ctx, name, attrs);
        break;
    case OTHER_PARSER_PACKAGE:
        other_parser_package_start (ctx, name, attrs);
        break;
    default:
        break;
    }
}

static void
other_parser_package_end (OtherSAXContext *ctx, const char *name)
{
    SAXContext *sctx = &ctx->sctx;

    Package *p = sctx->current_package;

    g_assert (p != NULL);

    sctx->want_text = FALSE;

    if (!strcmp (name, "package")) {

        if (p->changelogs)
            p->changelogs = g_slist_reverse (p->changelogs);

        if (sctx->package_fn && !*sctx->error)
            sctx->package_fn (p, sctx->user_data);

        package_free (p);
        sctx->current_package = NULL;

        if (ctx->current_entry) {
            g_free (ctx->current_entry);
            ctx->current_entry = NULL;
        }

        ctx->state = OTHER_PARSER_TOPLEVEL;
    }

    else if (!strcmp (name, "changelog")) {
        ctx->current_entry->changelog =
            g_string_chunk_insert_len (p->chunk,
                                       sctx->text_buffer->str,
                                       sctx->text_buffer->len);

        p->changelogs = g_slist_prepend (p->changelogs, ctx->current_entry);
        ctx->current_entry = NULL;
    }
}

static void
other_sax_end_element (void *data, const char *name)
{
    OtherSAXContext *ctx = (OtherSAXContext *) data;
    SAXContext *sctx = &ctx->sctx;

    switch (ctx->state) {
    case OTHER_PARSER_PACKAGE:
        other_parser_package_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (sctx->text_buffer, 0);
}

static xmlSAXHandler other_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) other_sax_start_element, /* startElement */
    (endElementSAXFunc) other_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    sax_warning,      /* warning */
    sax_error,      /* error */
    sax_error,      /* fatalError */
};

void
yum_xml_parse_other (const char *filename,
                     CountFn count_callback,
                     PackageFn package_callback,
                     gpointer user_data,
                     GError **err)
{
    OtherSAXContext ctx;
    SAXContext *sctx = &ctx.sctx;

    int rc;

    ctx.state = OTHER_PARSER_TOPLEVEL;
    ctx.current_entry = NULL;
    
    sax_context_init(sctx, "other.xml", count_callback, package_callback,
                     user_data, err);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&other_sax_handler, &ctx, filename);

    if (sctx->current_package) {
        g_warning ("Incomplete package lost");
        package_free (sctx->current_package);
    }

    if (ctx.current_entry)
        g_free (ctx.current_entry);

    g_string_free (sctx->text_buffer, TRUE);
}
