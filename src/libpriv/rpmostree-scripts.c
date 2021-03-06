/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gio/gio.h>
#include "rpmostree-output.h"
#include "rpmostree-bwrap.h"
#include <err.h>
#include "libglnx.h"

#include "rpmostree-scripts.h"

typedef struct {
    const char *desc;
    rpmsenseFlags sense;
    rpmTagVal tag;
    rpmTagVal progtag;
    rpmTagVal flagtag;
} KnownRpmScriptKind;

#if 0
static const KnownRpmScriptKind ignored_scripts[] = {
  /* Ignore all of the *un variants since we never uninstall
   * anything in the RPM sense.
   */
  { "%preun", 0,
    RPMTAG_PREUN, RPMTAG_PREUNPROG, RPMTAG_PREUNFLAGS },
  { "%postun", 0,
    RPMTAG_POSTUN, RPMTAG_POSTUNPROG, RPMTAG_POSTUNFLAGS },
  { "%triggerun", RPMSENSE_TRIGGERUN,
    RPMTAG_TRIGGERUN, 0, 0 },
  { "%triggerpostun", RPMSENSE_TRIGGERPOSTUN,
    RPMTAG_TRIGGERPOSTUN, 0, 0 },
};
#endif

static const KnownRpmScriptKind pre_scripts[] = {
  { "%prein", 0,
    RPMTAG_PREIN, RPMTAG_PREINPROG, RPMTAG_PREINFLAGS },
};

static const KnownRpmScriptKind posttrans_scripts[] = {
  /* For now, we treat %post as equivalent to %posttrans */
  { "%post", 0,
    RPMTAG_POSTIN, RPMTAG_POSTINPROG, RPMTAG_POSTINFLAGS },
  { "%posttrans", 0,
    RPMTAG_POSTTRANS, RPMTAG_POSTTRANSPROG, RPMTAG_POSTTRANSFLAGS },
};

static const KnownRpmScriptKind unsupported_scripts[] = {
  { "%pretrans", 0,
    RPMTAG_PRETRANS, RPMTAG_PRETRANSPROG, RPMTAG_PRETRANSFLAGS },
  { "%triggerprein", RPMSENSE_TRIGGERPREIN,
    RPMTAG_TRIGGERPREIN, 0, 0 },
  { "%triggerin", RPMSENSE_TRIGGERIN,
    RPMTAG_TRIGGERIN, 0, 0 },
  { "%verify", 0,
    RPMTAG_VERIFYSCRIPT, RPMTAG_VERIFYSCRIPTPROG, RPMTAG_VERIFYSCRIPTFLAGS},
};

static RpmOstreeScriptAction
lookup_script_action (DnfPackage *package,
                      GHashTable *ignored_scripts,
                      const char *scriptdesc)
{
  const char *pkg_script = glnx_strjoina (dnf_package_get_name (package), ".", scriptdesc+1);
  const struct RpmOstreePackageScriptHandler *handler = rpmostree_script_gperf_lookup (pkg_script, strlen (pkg_script));
  if (ignored_scripts && g_hash_table_contains (ignored_scripts, pkg_script))
    return RPMOSTREE_SCRIPT_ACTION_IGNORE;
  if (!handler)
    return RPMOSTREE_SCRIPT_ACTION_DEFAULT;
  return handler->action;
}

gboolean
rpmostree_script_txn_validate (DnfPackage    *package,
                               Header         hdr,
                               GHashTable    *override_ignored_scripts,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (unsupported_scripts); i++)
    {
      const char *desc = unsupported_scripts[i].desc;
      rpmTagVal tagval = unsupported_scripts[i].tag;
      rpmTagVal progtagval = unsupported_scripts[i].progtag;
      RpmOstreeScriptAction action;

      if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
        continue;

      action = lookup_script_action (package, override_ignored_scripts, desc);
      switch (action)
        {
        case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Package '%s' has (currently) unsupported script of type '%s'",
                         dnf_package_get_name (package), desc);
            goto out;
          }
        case RPMOSTREE_SCRIPT_ACTION_IGNORE:
          continue;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
run_script_in_bwrap_container (int rootfs_fd,
                               const char *name,
                               const char *scriptdesc,
                               const char *script,
                               GCancellable  *cancellable,
                               GError       **error)
{
  gboolean ret = FALSE;
  const char *pkg_script = glnx_strjoina (name, ".", scriptdesc+1);
  const char *postscript_name = glnx_strjoina ("/", pkg_script);
  const char *postscript_path_container = glnx_strjoina ("/usr", postscript_name);
  const char *postscript_path_host = postscript_path_container + 1;
  g_autoptr(RpmOstreeBwrap) bwrap = NULL;
  gboolean created_var_tmp = FALSE;

  /* TODO - Create a pipe and send this to bwrap so it's inside the
   * tmpfs.  Note the +1 on the path to skip the leading /.
   */
  if (!glnx_file_replace_contents_at (rootfs_fd, postscript_path_host,
                                      (guint8*)script, -1,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      NULL, error))
    {
      g_prefix_error (error, "Writing script to %s: ", postscript_path_host);
      goto out;
    }
  if (fchmodat (rootfs_fd, postscript_path_host, 0755, 0) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* We need to make the mount point in the case where we're doing
   * package layering, since the host `/var` tree is empty.  We
   * *could* point at the real `/var`...but that seems
   * unnecessary/dangerous to me.  Daemons that need to perform data
   * migrations should do them as part of their systemd units and not
   * in %post.
   *
   * Another alternative would be to make a tmpfs with the compat
   * symlinks.
   */
  if (mkdirat (rootfs_fd, "var/tmp", 0755) < 0)
    {
      if (errno == EEXIST)
        ;
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    created_var_tmp = TRUE;

  bwrap = rpmostree_bwrap_new (rootfs_fd, RPMOSTREE_BWRAP_MUTATE_ROFILES, error,
                               /* Scripts can see a /var with compat links like alternatives */
                               "--ro-bind", "./var", "/var",
                               /* But no need to access persistent /tmp, so make it /tmp */
                               "--bind", "/tmp", "/var/tmp",
                               /* Allow RPM scripts to change the /etc defaults */
                               "--symlink", "usr/etc", "/etc",
                               NULL);
  if (!bwrap)
    goto out;

  rpmostree_bwrap_append_child_argv (bwrap,
                                     postscript_path_container,
                                     /* http://www.rpm.org/max-rpm/s1-rpm-inside-scripts.html#S3-RPM-INSIDE-PRE-SCRIPT */
                                     "1",
                                     NULL);

  if (!rpmostree_bwrap_run (bwrap, error))
    goto out;

  ret = TRUE;
 out:
  if (created_var_tmp)
    (void) unlinkat (rootfs_fd, "var/tmp", AT_REMOVEDIR);
  return ret;
}

static gboolean
run_known_rpm_script (const KnownRpmScriptKind *rpmscript,
                      DnfPackage    *pkg,
                      Header         hdr,
                      GHashTable    *ignore_scripts,
                      int            rootfs_fd,
                      GCancellable  *cancellable,
                      GError       **error)
{
  const char *desc = rpmscript->desc;
  rpmTagVal tagval = rpmscript->tag;
  rpmTagVal progtagval = rpmscript->progtag;
  const char *script;
  g_autofree char **args = NULL;
  RpmOstreeScriptAction action;
  struct rpmtd_s td;

  if (!(headerIsEntry (hdr, tagval) || headerIsEntry (hdr, progtagval)))
    return TRUE;

  script = headerGetString (hdr, tagval);
  if (!script)
    return TRUE;

  if (headerGet (hdr, progtagval, &td, (HEADERGET_ALLOC|HEADERGET_ARGV)))
    args = td.data;

  action = lookup_script_action (pkg, ignore_scripts, desc);
  switch (action)
    {
    case RPMOSTREE_SCRIPT_ACTION_DEFAULT:
      {
        static const char lua[] = "<lua>";
        if (args && args[0] && strcmp (args[0], lua) == 0)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Package '%s' has (currently) unsupported %s script in '%s'",
                         dnf_package_get_name (pkg), lua, desc);
            return FALSE;
          }
        if (!run_script_in_bwrap_container (rootfs_fd, dnf_package_get_name (pkg), desc, script,
                                            cancellable, error))
          {
            g_prefix_error (error, "Running %s for %s: ", desc, dnf_package_get_name (pkg));
            return FALSE;
          }
        break;
      }
    case RPMOSTREE_SCRIPT_ACTION_IGNORE:
      return TRUE;
    }

  return TRUE;
}

gboolean
rpmostree_posttrans_run_sync (DnfPackage    *pkg,
                              Header         hdr,
                              GHashTable    *ignore_scripts,
                              int            rootfs_fd,
                              GCancellable  *cancellable,
                              GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (posttrans_scripts); i++)
    {
      if (!run_known_rpm_script (&posttrans_scripts[i], pkg, hdr, ignore_scripts,
                                 rootfs_fd, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_pre_run_sync (DnfPackage    *pkg,
                        Header         hdr,
                        GHashTable    *ignore_scripts,
                        int            rootfs_fd,
                        GCancellable  *cancellable,
                        GError       **error)
{
  for (guint i = 0; i < G_N_ELEMENTS (pre_scripts); i++)
    {
      if (!run_known_rpm_script (&pre_scripts[i], pkg, hdr, ignore_scripts,
                                 rootfs_fd, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
rpmostree_script_ignore_hash_from_strv (const char *const *strv,
                                        GHashTable **out_hash,
                                        GError **error)
{
  g_autoptr(GHashTable) ignore_scripts = NULL;
  if (!strv)
    {
      *out_hash = NULL;
      return TRUE;
    }
  ignore_scripts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (const char *const* iter = strv; iter && *iter; iter++)
    g_hash_table_add (ignore_scripts, g_strdup (*iter));
  *out_hash = g_steal_pointer (&ignore_scripts);
  return TRUE;
}
