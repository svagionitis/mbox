#include "defs.h"
#include "sbox.h"
#include "dbg.h"
#include "fsmap.h"

#include <err.h>
#include <dirent.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

struct linux_dirent {
    long           d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    char           d_name[];
};

#define min(a, b) ((a) < (b)? (a): (b))

/* os global structure */
static struct fsmap* os_deleted_fs = NULL; /* deleted fs map */

static inline
int sbox_is_deleted(char *path) 
{
    return is_deleted(os_deleted_fs, path);
}

static inline
int sbox_delete_path(char *path) 
{
    add_path_to_fsmap(&os_deleted_fs, path, PATH_DELETED);
    return 1;
}

void sbox_cleanup(void)
{
    if (os_deleted_fs) {
        free_fsmap(os_deleted_fs);
    }
}

static
void sbox_setenv(void)
{
    char spwd[PATH_MAX];
    char hpwd[PATH_MAX];

    getcwd(hpwd, sizeof(hpwd));

    // setenvs for test scripts
    if (!getenv("SPWD")) {
        snprintf(spwd, sizeof(spwd), "%s/%s", opt_root, hpwd);
        setenv("SPWD", spwd, 1);
    }
    if (!getenv("HPWD")) {
        setenv("HPWD", hpwd, 1);
    }
}

void sbox_check_test_cond(const char *pn, const char *key)
{
    sbox_setenv();

    FILE *fp = fopen(pn , "r");
    if (!fp) {
        err(1, "fopen");
    }

    char m1[128];
    char m2[128];
    snprintf(m1, sizeof(m1), "# %s:", key);
    snprintf(m2, sizeof(m2), "#%s:", key);

    size_t len = 0;
    char *line = NULL;
    while (getline(&line, &len, fp) != -1) {
        if (strncmp(line, m1, strlen(m1)) == 0 ||
            strncmp(line, m2, strlen(m2)) == 0) {
            char *cmd = strchr(line, ':');
            cmd ++;
            cmd[strlen(cmd)-1] = '\0';
            dbg(testcond, "Check %s: %s", key, cmd);
            if (system(cmd) != 0) {
                dbg(info, "Failed to check %s condition: %s", key, cmd);
                exit(1);
            }
        }
    }

    fclose(fp);
}

int get_fd_path(int pid, int fd, char *path, int len)
{
    ssize_t read;
    char proc[PATH_MAX];

    snprintf(proc, sizeof(proc), "/proc/%d/fd/%d", pid, fd);
    if ((read = readlink(proc, path, len - 1)) < 0) {
        /* fd doesn't exist*/
        path[0] = '\0';
        return 0;
    }
    path[read] = '\0';
    dbg(test, "> %s", path);
    return 1;
}

int get_cwd_path(int pid, char *path, int len)
{
    ssize_t read;
    char proc[PATH_MAX];

    snprintf(proc, sizeof(proc), "/proc/%d/cwd", pid);
    if ((read = readlink(proc, path, len - 1)) < 0) {
        err(1, "proc/cwd");
    }
    path[read] = '\0';

    // check if cwd is under sboxfs
    if (is_in_sboxfs(path)) {
        char *iter;
        for (iter = path; *(iter + opt_root_len) != '\0'; iter ++) {
            *iter = *(iter + opt_root_len);
        }
        *iter = '\0';
        dbg(test, "cwd in sboxfs: %s", path);
    }

    return 1;
}

//
// return a path relative to fd from a syscall
//
void get_hpn_from_fd_and_arg(struct tcb *tcp, int fd, int arg, char *path, int len)
{
    // ugly realpath requirement
    assert(len == PATH_MAX);

    char pn[PATH_MAX];
    const long ptr = tcp->u_arg[arg];
    if (umovestr(tcp, ptr, PATH_MAX, pn) <= 0) {
        pn[0] = '\0';
    }

    // abspath
    if (pn[0] == '/') {
        strncpy(path, pn, len);
        normalize_path(path);
        return;
    }

    // relpath, so resolve it
    char root[PATH_MAX];
    if (fd == AT_FDCWD) {
        // read /proc/pid/cwd
        get_cwd_path(tcp->pid, root, sizeof(root));
    } else {
        // read /proc/pid/fd/#
        get_fd_path(tcp->pid, fd, root, sizeof(root));
    }

    snprintf(path, len, "%s/%s", root, pn);
    normalize_path(path);
    
    // dbg(test, "XXX %s -> %s", pn, path);
}

void get_spn_from_hpn(char *hpn, char *spn, int len)
{
    snprintf(spn, len, "%s%s", opt_root, hpn);
}

void set_regs_with_arg(struct user_regs_struct *regs, int arg, long val)
{
    switch (arg) {
    case 0: regs->rdi = val; break;
    case 1: regs->rsi = val; break;
    case 2: regs->rdx = val; break;
    case 3: regs->r10 = val; break;
    case 4: regs->r8  = val; break;
    case 5: regs->r9  = val; break;
    case 6: regs->rax = val; break;
    default:
        dbg(fatal, "Unknown argument: %d\n", arg);
        exit(1);
    }
}

void sbox_remote_write(struct tcb *tcp, long ptr, char *buf, int len)
{
    struct iovec local[1], remote[1];

    local[0].iov_base = (void*)buf;
    local[0].iov_len = len;
    remote[0].iov_base = (void*)ptr;
    remote[0].iov_len = len;

    if (process_vm_writev(tcp->pid, local, 1, remote, 1, 0) < 0) {
        err(1, "writev failed: pid=%d", tcp->pid);
    }
}

void sbox_rewrite_arg(struct tcb *tcp, int arg, long val)
{
    struct user_regs_struct *regs = &tcp->regs;
    set_regs_with_arg(regs, arg, val);
    ptrace(PTRACE_SETREGS, tcp->pid, 0, regs);
}

void sbox_rewrite_ret(struct tcb *tcp, long long ret) 
{
    if (ret == 0) {
        tcp->u_error = 0;
    }

    tcp->u_rval = ret;
    sbox_rewrite_arg(tcp, ARG_RET, ret);
}

void sbox_hijack_str(struct tcb *tcp, int arg, char *new)
{
    struct user_regs_struct *regs = &tcp->regs;

    int n = tcp->hijacked;
    tcp->hijacked_args[n] = arg;
    tcp->hijacked_vals[n] = tcp->u_arg[arg];
    tcp->hijacked ++;

    /* XXX. need to find the readonly memory */
    long new_ptr = regs->rsp - PATH_MAX * (arg+1);
    sbox_remote_write(tcp, new_ptr, new, strlen(new)+1);
    sbox_rewrite_arg(tcp, arg, new_ptr);
}


void sbox_hijack_arg(struct tcb *tcp, int arg, long new)
{
    int n = tcp->hijacked;
    tcp->hijacked_args[n] = arg;
    tcp->hijacked_vals[n] = tcp->u_arg[arg];
    tcp->hijacked ++;

    sbox_rewrite_arg(tcp, arg, new);
}

void sbox_restore_hijack(struct tcb *tcp)
{
    int i;
    for (i = 0; i < tcp->hijacked; i ++) {
        sbox_rewrite_arg(tcp, tcp->hijacked_args[i], tcp->hijacked_vals[i]);
    }
    tcp->hijacked = 0;
}

void sbox_sync_parent_dirs(char *hpn, char *spn)
{
    // already synced
    if (exists_parent_dir(spn)) {
        return;
    }

    // don't have to sync
    if (!exists_parent_dir(hpn)) {
        return;
    }

    dbg(path, "sync path '%s'", hpn);

    // find the last / and split for a while
    char *last = spn + strlen(spn);
    for (; *last != '/' && last >= spn; last --);
    if (*last != '/') {
        return;
    }

    // split spn to iterate
    *last = '\0';

    int done = 0;
    char *iter = spn + opt_root_len;

    while (!done && *(++iter) != '\0') {
        // find next '/' or '\0'
        for (; *iter != '\0' && *iter != '/'; iter ++);
        // done
        if (*iter == '\0') {
            done = 1;
        }
        // make a dir
        *iter = '\0';
        // fetch the mode
        struct stat hpn_stat;
        if (stat(spn + opt_root_len, &hpn_stat) < 0) {
            break;
        }
        mkdir(spn, hpn_stat.st_mode);
        if (done) {
            break;
        }
        // continue
        *iter = '/';
    }

    // restore
    *last = '/';
}

static
void sbox_open_enter(struct tcb *tcp, int fd, int arg, int oflag)
{
    int accmode;
    char hpn[PATH_MAX];
    char spn[PATH_MAX];

    get_hpn_from_fd_and_arg(tcp, fd, arg, hpn, PATH_MAX);
    get_spn_from_hpn(hpn, spn, PATH_MAX);

    // NOTE. ignore /dev and /proc
    //   /proc: need to emulate /proc/pid/fd/*
    //   /dev : need to verify what is correct to do
    if (strncmp(hpn, "/dev/", 5) == 0 || strncmp(hpn, "/proc/", 6) == 0) {
        return;
    }

    // if the path is deleted
    if (sbox_is_deleted(hpn)) {
        sbox_sync_parent_dirs(hpn, spn);
        sbox_hijack_str(tcp, arg, spn);
        return;
    }

    // whenever path exists in the sandbox, go to there
    if (path_exists(spn)) {
        sbox_hijack_str(tcp, arg, spn);
        return;
    }

    // readonly, just use hostfs
    accmode = oflag & O_ACCMODE;
    if (accmode == O_RDONLY) {
        return;
    }

    // trunc
    if (oflag & O_TRUNC) {
        sbox_sync_parent_dirs(hpn, spn);
        sbox_hijack_str(tcp, arg, spn);
        return;
    }

    // write or read/write
    if (accmode == O_RDWR || accmode == O_RDWR) {
        sbox_sync_parent_dirs(hpn, spn);
        copyfile(hpn, spn);
        sbox_hijack_str(tcp, arg, spn);
    }
}

int sbox_open(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_open_enter(tcp, AT_FDCWD, 0, tcp->u_arg[1]);
    }
    return 0;
}

int sbox_openat(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_open_enter(tcp, tcp->u_arg[0], 1, tcp->u_arg[2]);
    }
    return 0;
}

int sbox_rewrite_path(struct tcb *tcp, int fd, int arg, int flag)
{
    char hpn[PATH_MAX];
    char spn[PATH_MAX];

    get_hpn_from_fd_and_arg(tcp, fd, arg, hpn, PATH_MAX);
    get_spn_from_hpn(hpn, spn, PATH_MAX);

    // satisfying one of rewrite conditions
    if (flag != RW_NONE                     \
        || sbox_is_deleted(hpn) \
        || path_exists(spn)) {
        
        // to be written to spn, so sync parent paths
        if (flag != RW_NONE) {
            sbox_sync_parent_dirs(hpn, spn);
        }

        // writing intent (not force)
        if (flag == RW_WRITING) {
            copyfile(hpn, spn);
        }

        // finally hijack path (arg)
        sbox_hijack_str(tcp, arg, spn);

        dbg(path, "rewrite to %s", spn);
    }

    return 0;
}

int sbox_stat(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, AT_FDCWD, 0, RW_NONE);
    }
    return 0;
}

int sbox_newfstatat(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, tcp->u_arg[0], 1, RW_NONE);
    }
    return 0;
}

int sbox_mkdir(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, AT_FDCWD, 0, RW_FORCE);
    }
    return 0;
}

int sbox_mkdirat(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, tcp->u_arg[0], 1, RW_FORCE);
    }
    return 0;
}

int sbox_rmdir(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, AT_FDCWD, 0, RW_FORCE);
    } else {
        /* XXX */
    }
    return 0;
}

int sbox_unlink_general(struct tcb *tcp, int fd, int arg)
{
    char hpn[PATH_MAX];
    char spn[PATH_MAX];

    if (entering(tcp)) {
        sbox_rewrite_path(tcp, fd, arg, RW_FORCE);
    } else {
        // get hpn/spn
        get_hpn_from_fd_and_arg(tcp, fd, arg, hpn, PATH_MAX);
        get_spn_from_hpn(hpn, spn, PATH_MAX);
        
        // failed on sandbox
        if ((long)tcp->regs.rax < 0) {
            // emulate successful deletion
            if (!sbox_is_deleted(hpn) && path_exists(hpn)) {
                dbg(path, "emulate successful unlink: %s", hpn);
                sbox_rewrite_ret(tcp, 0);
            }
        }

        // mark the file deleted
        if ((long)tcp->regs.rax == 0) {
            sbox_delete_path(hpn);
        }
    }
    return 0;
}

int sbox_unlink(struct tcb *tcp)
{
    return sbox_unlink_general(tcp, AT_FDCWD, 0);
}

int sbox_unlinkat(struct tcb *tcp)
{
    return sbox_unlink_general(tcp, tcp->u_arg[0], 1);
}

int sbox_access_general(struct tcb *tcp, int fd, int arg)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, fd, arg, RW_NONE);
    }
    return 0;
}

int sbox_access(struct tcb *tcp)
{
    return sbox_access_general(tcp, AT_FDCWD, 0);
}

int sbox_faccessat(struct tcb *tcp)
{
    return sbox_access_general(tcp, tcp->u_arg[0], 1);
}

int sbox_getdents(struct tcb *tcp)
{
    static char buf[4096];
    static char tmp[4096];

    char spn[PATH_MAX];
    char hpn[PATH_MAX];

    // after pumping files on sandboxfs
    if (exiting(tcp) && tcp->regs.rax == 0) {
        int hostfd = tcp->u_arg[0];

        // just done with sandboxfs
        if (tcp->dentfd_sbox == -1) {
            // get hpn
            if (!get_fd_path(tcp->pid, hostfd, spn, sizeof(spn))) {
                // wrong fd anyway
                return 0;
            }

            // check if calling to sandboxfs
            if (strncmp(spn, opt_root, opt_root_len) != 0) {
                // return if calls on hostfs
                return 0;
            }

            strncpy(hpn, spn + opt_root_len, sizeof(hpn));
            dbg(getdents, "spn:%s", hpn);
            dbg(getdents, "hpn:%s", hpn);

            strncpy(tcp->dentfd_spn, spn, sizeof(tcp->dentfd_spn));

            tcp->dentfd_host = hostfd;
            tcp->dentfd_sbox = open(hpn, O_RDONLY | O_DIRECTORY);
            if (tcp->dentfd_sbox < 0) {
                return 0;
            }
        }

        // NOTE. we only support, single contiguous getdent()
        if (tcp->dentfd_host != hostfd) {
            dbg(fatal, "we only support a single getdent() at a time");
            exit(1);
        }

        // manually invoke getdents on hostfs.
        // to overwrite less than the memory of tracee (dirp), we use
        // buf with the size less that the given value (count).
        int len = syscall(SYS_getdents, tcp->dentfd_sbox, buf,
                          min(sizeof(buf), tcp->u_arg[2]));

        // done with pumping dirs of sandboxfs
        if (len == 0) {
            close(tcp->dentfd_sbox);
            tcp->dentfd_sbox = -1;
            tcp->dentfd_host = -1;
            return 0;
        }

        // filter dir contents
        int dst_iter = 0;
        int src_iter = 0;
        while (src_iter < len) {
            struct linux_dirent *d = (struct linux_dirent *)(buf + src_iter);
            // ignore . and ..
            if (d->d_name[0] == '.') {
                if (d->d_name[1] == '\0' ||
                    (d->d_name[1] == '.' && d->d_name[2] == '\0')) {
                    src_iter += d->d_reclen;
                    continue;
                }
            }

            // ignore dentry if exists in sandboxfs
            snprintf(hpn, sizeof(hpn), "%s/%s", tcp->dentfd_spn, d->d_name);
            if (path_exists(hpn)) {
                dbg(getdents, "[%3d] found in sbox: %s", src_iter, hpn);
                src_iter += d->d_reclen;
                continue;
            }

            // copy to dest
            memcpy(tmp + dst_iter, buf + src_iter, d->d_reclen);
            src_iter += d->d_reclen;
            dst_iter += d->d_reclen;
        }

        // copy buf/ret to tracee
        sbox_rewrite_ret(tcp, dst_iter);
        sbox_remote_write(tcp, tcp->u_arg[1], tmp, dst_iter);
    }

    return 0;
}

/*
 * allows chdir into sboxfs or hostfs since we sanitize getcwd()
 * 
 * NOTE. fchdir() doesn't need to be handled because open() already
 * rewrites the path if needed.
 */
int sbox_chdir(struct tcb *tcp)
{
    if (entering(tcp)) {
        sbox_rewrite_path(tcp, AT_FDCWD, 0, RW_NONE);
    }
    return 0;
}

int sbox_getcwd(struct tcb *tcp)
{
    // ret = len(buf)
    const long ret = tcp->regs.rax;

    if (exiting(tcp) &&  ret > 0) {
        char pn[PATH_MAX];
        const long ptr = tcp->u_arg[0];

        if (umovestr(tcp, ptr, PATH_MAX, pn) <= 0) {
            err(1, "failed to copy string from getcwd buf");
        }

        if (is_in_sboxfs(pn)) {
            char *hpn = pn + opt_root_len;
            sbox_remote_write(tcp, ptr, hpn, strlen(hpn)+1);
            sbox_rewrite_ret(tcp, ret - opt_root_len);

            dbg(test, "XXX: getcwd from sbox: %s", hpn);
        }
    }
    return 0;
}

/* interactive mode */

static
void _sbox_walk(const char *root, const char *name,
                int (*handler)(char *spn, char *hpn))
{
    char pn[PATH_MAX];
    if (name) {
        snprintf(pn, sizeof(pn), "%s/%s", root, name);
    } else {
        strncpy(pn, root, sizeof(pn));
    }

    DIR *dir = opendir(pn);
    if (!dir) {
        err(1, "opendir");
    }

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        const char *n = d->d_name;
        if (d->d_type & DT_DIR) {
            if ((n[0] == '.' && n[1] == '\0') ||
                (n[0] == '.' && n[1] == '.' && n[2] == '\0')) {
                continue;
            }
            _sbox_walk(pn, n, handler);
        } else {
            char spn[PATH_MAX];
            char hpn[PATH_MAX];
            snprintf(spn, sizeof(spn), "%s/%s", pn, n);
            strncpy(hpn, spn + opt_root_len, sizeof(hpn));

            if (handler) {
                handler(spn, hpn);
            }
        }
    }

    closedir(dir);
}

static
char _prompt(const char *menu)
{
    char c;
    printf(" %s ? > ", menu);
    c = kbhit();
    printf("\n");
    return c;
}

static
int _sh_diff(char *a, char *b)
{
    int pid = fork();
    if (!pid) {
        execlp("diff", "diff", "-urN", a, b, NULL);
        err(1, "diff");
    }
    waitpid(pid, NULL, 0);
    return 0;
}

static
int _sh_commit(char *spn, char *hpn)
{
    printf("  > Commiting %s\n", hpn);
    copyfile(spn, hpn);
    return 0;
}

static
int _sbox_interactive_menu(char *spn, char *hpn)
{
    static int opt_commit_all = 0;

    const char *menu \
        = "[C]:commit all, [c]:commit, [i]:ignore, [d]:diff, [q]:quit";

    if (opt_commit_all) {
        _sh_commit(spn, hpn);
        return 0;
    }

    while (1) {
        printf("F:%s\n", hpn);
        switch (_prompt(menu)) {
        case 'C':
            opt_commit_all = 1;
            /* fall-in */
        case 'c':
            _sh_commit(spn, hpn);
            /* fall-in */
        case 'i':
            return 0;
            break;
        case 'd':
            _sh_diff(spn, hpn);
            break;
        case 'q':
            exit(0);
            break;
        }
    }
    return 0;
}

static
int _sbox_print_files(char *spn, char *hpn)
{
    printf(" > F: %s\n", spn);
    return 0;
}

int sbox_interactive(void)
{
    printf("%s:\n", opt_root);
    _sbox_walk(opt_root, NULL, _sbox_print_files);
    _sbox_walk(opt_root, NULL, _sbox_interactive_menu);

    return 0;
}