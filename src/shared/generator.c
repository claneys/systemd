/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stdio_ext.h>
#include <unistd.h>

#include "alloc-util.h"
#include "dropin.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "log.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "special.h"
#include "specifier.h"
#include "string-util.h"
#include "time-util.h"
#include "unit-name.h"
#include "util.h"

int generator_open_unit_file(
                const char *dest,
                const char *source,
                const char *name,
                FILE **file) {

        const char *unit;
        FILE *f;

        unit = strjoina(dest, "/", name);

        f = fopen(unit, "wxe");
        if (!f) {
                if (source && errno == EEXIST)
                        return log_error_errno(errno,
                                               "Failed to create unit file %s, as it already exists. Duplicate entry in %s?",
                                               unit, source);
                else
                        return log_error_errno(errno,
                                               "Failed to create unit file %s: %m",
                                               unit);
        }

        (void) __fsetlocking(f, FSETLOCKING_BYCALLER);

        fprintf(f,
                "# Automatically generated by %s\n\n",
                program_invocation_short_name);

        *file = f;
        return 0;
}

int generator_add_symlink(const char *dir, const char *dst, const char *dep_type, const char *src) {
        /* Adds a symlink from <dst>.<dep_type>/ to <src> (if src is absolute)
         * or ../<src> (otherwise). */

        const char *from, *to;

        from = path_is_absolute(src) ? src : strjoina("../", src);
        to = strjoina(dir, "/", dst, ".", dep_type, "/", basename(src));

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                if (errno != EEXIST)
                        return log_error_errno(errno, "Failed to create symlink \"%s\": %m", to);

        return 0;
}

static int write_fsck_sysroot_service(const char *dir, const char *what) {
        _cleanup_free_ char *device = NULL, *escaped = NULL, *escaped2 = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *unit;
        int r;

        escaped = specifier_escape(what);
        if (!escaped)
                return log_oom();

        escaped2 = cescape(escaped);
        if (!escaped2)
                return log_oom();

        unit = strjoina(dir, "/"SPECIAL_FSCK_ROOT_SERVICE);
        log_debug("Creating %s", unit);

        r = unit_name_from_path(what, ".device", &device);
        if (r < 0)
                return log_error_errno(r, "Failed to convert device \"%s\" to unit name: %m", what);

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by %1$s\n\n"
                "[Unit]\n"
                "Description=File System Check on %2$s\n"
                "Documentation=man:systemd-fsck-root.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%3$s\n"
                "Conflicts=shutdown.target\n"
                "After=initrd-root-device.target local-fs-pre.target %3$s\n"
                "Before=shutdown.target\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" SYSTEMD_FSCK_PATH " %4$s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                escaped,
                device,
                escaped2);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return 0;
}

int generator_write_fsck_deps(
                FILE *f,
                const char *dir,
                const char *what,
                const char *where,
                const char *fstype) {

        int r;

        assert(f);
        assert(dir);
        assert(what);
        assert(where);

        if (!is_device_path(what)) {
                log_warning("Checking was requested for \"%s\", but it is not a device.", what);
                return 0;
        }

        if (!isempty(fstype) && !streq(fstype, "auto")) {
                r = fsck_exists(fstype);
                if (r < 0)
                        log_warning_errno(r, "Checking was requested for %s, but couldn't detect if fsck.%s may be used, proceeding: %m", what, fstype);
                else if (r == 0) {
                        /* treat missing check as essentially OK */
                        log_debug("Checking was requested for %s, but fsck.%s does not exist.", what, fstype);
                        return 0;
                }
        }

        if (path_equal(where, "/")) {
                const char *lnk;

                lnk = strjoina(dir, "/" SPECIAL_LOCAL_FS_TARGET ".wants/"SPECIAL_FSCK_ROOT_SERVICE);

                mkdir_parents(lnk, 0755);
                if (symlink(SYSTEM_DATA_UNIT_PATH "/"SPECIAL_FSCK_ROOT_SERVICE, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        } else {
                _cleanup_free_ char *_fsck = NULL;
                const char *fsck;

                if (in_initrd() && path_equal(where, "/sysroot")) {
                        r = write_fsck_sysroot_service(dir, what);
                        if (r < 0)
                                return r;

                        fsck = SPECIAL_FSCK_ROOT_SERVICE;
                } else {
                        r = unit_name_from_path_instance("systemd-fsck", what, ".service", &_fsck);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create fsck service name: %m");

                        fsck = _fsck;
                }

                fprintf(f,
                        "Requires=%1$s\n"
                        "After=%1$s\n",
                        fsck);
        }

        return 0;
}

int generator_write_timeouts(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts,
                char **filtered) {

        /* Configure how long we wait for a device that backs a mount point or a
         * swap partition to show up. This is useful to support endless device timeouts
         * for devices that show up only after user input, like crypto devices. */

        _cleanup_free_ char *node = NULL, *unit = NULL, *timeout = NULL;
        usec_t u;
        int r;

        r = fstab_filter_options(opts, "comment=systemd.device-timeout\0"
                                       "x-systemd.device-timeout\0",
                                 NULL, &timeout, filtered);
        if (r <= 0)
                return r;

        r = parse_sec_fix_0(timeout, &u);
        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", where, timeout);
                return 0;
        }

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();
        if (!is_device_path(node)) {
                log_warning("x-systemd.device-timeout ignored for %s", what);
                return 0;
        }

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        return write_drop_in_format(dir, unit, 50, "device-timeout",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "JobRunningTimeoutSec=%s",
                                    program_invocation_short_name,
                                    timeout);
}

int generator_write_device_deps(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts) {

        /* fstab records that specify _netdev option should apply the network
         * ordering on the actual device depending on network connection. If we
         * are not mounting real device (NFS, CIFS), we rely on _netdev effect
         * on the mount unit itself. */

        _cleanup_free_ char *node = NULL, *unit = NULL;
        int r;

        if (!fstab_test_option(opts, "_netdev\0"))
                return 0;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to apply dependencies to. */
        if (!is_device_path(node))
                return 0;

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       node);

        /* See mount_add_default_dependencies for explanation why we create such
         * dependencies. */
        return write_drop_in_format(dir, unit, 50, "netdev-dependencies",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "After=" SPECIAL_NETWORK_ONLINE_TARGET " " SPECIAL_NETWORK_TARGET "\n"
                                    "Wants=" SPECIAL_NETWORK_ONLINE_TARGET "\n",
                                    program_invocation_short_name);
}

int generator_write_initrd_root_device_deps(const char *dir, const char *what) {
        _cleanup_free_ char *unit = NULL;
        int r;

        r = unit_name_from_path(what, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       what);

        return write_drop_in_format(dir, SPECIAL_INITRD_ROOT_DEVICE_TARGET, 50, "root-device",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "Requires=%s\n"
                                    "After=%s",
                                    program_invocation_short_name,
                                    unit,
                                    unit);
}

int generator_hook_up_mkswap(
                const char *dir,
                const char *what) {

        _cleanup_free_ char *node = NULL, *unit = NULL, *escaped = NULL, *where_unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *unit_file;
        int r;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to work on. */
        if (!is_device_path(node))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format something that is not a device node: %s",
                                       node);

        r = unit_name_from_path_instance("systemd-mkswap", node, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       node);

        unit_file = strjoina(dir, "/", unit);
        log_debug("Creating %s", unit_file);

        escaped = cescape(node);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path(what, ".swap", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       what);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Make Swap on %%f\n"
                "Documentation=man:systemd-mkswap@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.device\n"
                "Conflicts=shutdown.target\n"
                "After=%%i.device\n"
                "Before=shutdown.target %s\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_MAKEFS_PATH " swap %s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                where_unit,
                escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit_file);

        return generator_add_symlink(dir, where_unit, "requires", unit);
}

int generator_hook_up_mkfs(
                const char *dir,
                const char *what,
                const char *where,
                const char *type) {

        _cleanup_free_ char *node = NULL, *unit = NULL, *escaped = NULL, *where_unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *unit_file;
        int r;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to work on. */
        if (!is_device_path(node))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format something that is not a device node: %s",
                                       node);

        if (!type || streq(type, "auto"))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Cannot format partition %s, filesystem type is not specified",
                                       node);

        r = unit_name_from_path_instance("systemd-makefs", node, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       node);

        unit_file = strjoina(dir, "/", unit);
        log_debug("Creating %s", unit_file);

        escaped = cescape(node);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path(where, ".mount", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       where);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Make File System on %%f\n"
                "Documentation=man:systemd-makefs@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.device\n"
                "Conflicts=shutdown.target\n"
                "After=%%i.device\n"
                /* fsck might or might not be used, so let's be safe and order
                 * ourselves before both systemd-fsck@.service and the mount unit. */
                "Before=shutdown.target systemd-fsck@%%i.service %s\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_MAKEFS_PATH " %s %s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                where_unit,
                type,
                escaped);
        // XXX: what about local-fs-pre.target?

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit_file);

        return generator_add_symlink(dir, where_unit, "requires", unit);
}

int generator_hook_up_growfs(
                const char *dir,
                const char *where,
                const char *target) {

        _cleanup_free_ char *unit = NULL, *escaped = NULL, *where_unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *unit_file;
        int r;

        escaped = cescape(where);
        if (!escaped)
                return log_oom();

        r = unit_name_from_path_instance("systemd-growfs", where, ".service", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit instance name from path \"%s\": %m",
                                       where);

        r = unit_name_from_path(where, ".mount", &where_unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path \"%s\": %m",
                                       where);

        unit_file = strjoina(dir, "/", unit);
        log_debug("Creating %s", unit_file);

        f = fopen(unit_file, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m",
                                       unit_file);

        fprintf(f,
                "# Automatically generated by %s\n\n"
                "[Unit]\n"
                "Description=Grow File System on %%f\n"
                "Documentation=man:systemd-growfs@.service(8)\n"
                "DefaultDependencies=no\n"
                "BindsTo=%%i.mount\n"
                "Conflicts=shutdown.target\n"
                "After=%%i.mount\n"
                "Before=shutdown.target %s\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart="SYSTEMD_GROWFS_PATH " %s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                target,
                escaped);

        return generator_add_symlink(dir, where_unit, "wants", unit);
}

int generator_enable_remount_fs_service(const char *dir) {
        /* Pull in systemd-remount-fs.service */
        return generator_add_symlink(dir, SPECIAL_LOCAL_FS_TARGET, "wants",
                                     SYSTEM_DATA_UNIT_PATH "/" SPECIAL_REMOUNT_FS_SERVICE);
}

void log_setup_generator(void) {
        log_set_prohibit_ipc(true);
        log_setup_service();
}
