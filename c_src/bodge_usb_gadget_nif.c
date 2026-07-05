// SPDX-License-Identifier: Apache-2.0
//
// bodge_usb_gadget fd shim NIF.
//
// A narrow NIF over a single file descriptor for gadget-side USB: FunctionFS
// ep0/endpoint files and kernel-function chardevs (hidg, ttyGS). open/close/
// read/write plus the blocking variants FunctionFS needs. The fd lives in an
// ErlNifResource whose destructor closes it, so a dropped or GC'd handle never
// leaks an fd. errno is captured immediately after each syscall and mapped to
// an atom.
//
// Linux only. read/write run inline on a normal scheduler and assume a fast or
// non-blocking fd. read_blocking/write_blocking are for FunctionFS endpoint
// files, which block until the host transacts and are not pollable: they run
// on dirty I/O schedulers and must not hold the fd lock across the syscall --
// they take a refcount and release it (begin_blocking/end_blocking), so
// close/1 during a blocked call defers fd teardown instead of racing it.

#define _GNU_SOURCE // O_CLOEXEC and friends under -std=c11

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <erl_nif.h>

typedef struct {
    int fd;             // -1 once closed
    ErlNifMutex *lock;  // serializes fd use vs close (no double-close/UAF)
    int select_active;  // enif_select(READ) currently armed
    int closing;        // close requested; fd torn down once quiescent
    int busy;           // blocking reads/writes in flight that released the lock
} GadgetFd;

static ErlNifResourceType *gadget_fd_type = NULL;

// Atoms, initialized in load().
static ERL_NIF_TERM am_ok;
static ERL_NIF_TERM am_error;
static ERL_NIF_TERM am_ebadf;

static void teardown_fd(GadgetFd *r) {
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
}

// Finalize a deferred close: actually tear the fd down once nothing is using it
// (no select armed, no blocking call in flight). Call with r->lock held.
static void try_close_locked(GadgetFd *r) {
    if (r->closing && !r->select_active && r->busy == 0)
        teardown_fd(r);
}

// Take a reference for a blocking syscall and hand back the fd to use WITHOUT
// holding the lock across it -- otherwise a normal-scheduler NIF (close) on the
// same handle would stall behind it, violating the scheduler contract. Returns
// -1 if the fd is closed or a close is pending.
static int begin_blocking(GadgetFd *r) {
    enif_mutex_lock(r->lock);
    if (r->fd < 0 || r->closing) {
        enif_mutex_unlock(r->lock);
        return -1;
    }
    r->busy++;
    int fd = r->fd;
    enif_mutex_unlock(r->lock);
    return fd;
}

// Release a begin_blocking() reference; finalize a close that waited on us.
static void end_blocking(GadgetFd *r) {
    enif_mutex_lock(r->lock);
    r->busy--;
    try_close_locked(r);
    enif_mutex_unlock(r->lock);
}

static ERL_NIF_TERM mk_atom(ErlNifEnv *env, const char *s) {
    ERL_NIF_TERM a;
    if (enif_make_existing_atom(env, s, &a, ERL_NIF_LATIN1))
        return a;
    return enif_make_atom(env, s);
}

// Map errno -> atom. Common cases get a name; anything else becomes :eNNN so the
// caller always receives an atom and never loses information.
static ERL_NIF_TERM errno_atom(ErlNifEnv *env, int e) {
    const char *name = NULL;
    switch (e) {
    case EPERM:      name = "eperm"; break;
    case ENOENT:     name = "enoent"; break;
    case EINTR:      name = "eintr"; break;
    case EIO:        name = "eio"; break;
    case ENXIO:      name = "enxio"; break;
    case EBADF:      name = "ebadf"; break;
    case EAGAIN:     name = "eagain"; break;
    case ENOMEM:     name = "enomem"; break;
    case EACCES:     name = "eacces"; break;
    case EFAULT:     name = "efault"; break;
    case EBUSY:      name = "ebusy"; break;
    case ENODEV:     name = "enodev"; break;
    case EINVAL:     name = "einval"; break;
    case ENOTTY:     name = "enotty"; break;
    case EPIPE:      name = "epipe"; break;
    case ENOSPC:     name = "enospc"; break;
    case ESHUTDOWN:  name = "eshutdown"; break;  // gadget unbound mid-I/O
    case EPROTO:     name = "eproto"; break;
    case ETIMEDOUT:  name = "etimedout"; break;
    case EMFILE:     name = "emfile"; break;
    case ENFILE:     name = "enfile"; break;
    default: break;
    }
    if (name)
        return mk_atom(env, name);
    char buf[16];
    snprintf(buf, sizeof(buf), "e%d", e);
    return enif_make_atom(env, buf);
}

static ERL_NIF_TERM err_tuple(ErlNifEnv *env, int e) {
    return enif_make_tuple2(env, am_error, errno_atom(env, e));
}

// enif_select stop callback: runs when it is safe to close the fd (no in-flight
// select). This is where an fd that was ever selected upon is actually closed.
static void gadget_fd_stop(ErlNifEnv *env, void *obj, ErlNifEvent event, int is_direct_call) {
    (void)env;
    (void)event;
    (void)is_direct_call;
    GadgetFd *r = (GadgetFd *)obj;
    enif_mutex_lock(r->lock);
    r->select_active = 0;
    // A blocking call may still be mid-syscall (it released the lock); if so,
    // its end_blocking() does the teardown. Otherwise finalize here.
    if (r->busy == 0)
        teardown_fd(r);
    enif_mutex_unlock(r->lock);
}

static void gadget_fd_dtor(ErlNifEnv *env, void *obj) {
    (void)env;
    GadgetFd *r = (GadgetFd *)obj;
    // If the fd was ever selected upon, the stop callback already tore it down
    // (enif_select keeps the resource alive until STOP completes, so stop runs
    // before dtor). Otherwise close it here. No other references remain.
    if (r->fd >= 0)
        teardown_fd(r);
    if (r->lock) {
        enif_mutex_destroy(r->lock);
        r->lock = NULL;
    }
}

// Translate a list of flag atoms to open(2) flags. O_CLOEXEC is always set so
// descriptors never leak across an exec. Returns -1 on a bad flag.
static int parse_open_flags(ErlNifEnv *env, ERL_NIF_TERM list, int *out) {
    int flags = O_CLOEXEC;
    int have_access = 0;
    ERL_NIF_TERM head, tail = list;
    char name[16];

    while (enif_get_list_cell(env, tail, &head, &tail)) {
        if (enif_get_atom(env, head, name, sizeof(name), ERL_NIF_LATIN1) <= 0)
            return -1;
        if (strcmp(name, "rdonly") == 0)      { flags |= O_RDONLY; have_access = 1; }
        else if (strcmp(name, "wronly") == 0) { flags |= O_WRONLY; have_access = 1; }
        else if (strcmp(name, "rdwr") == 0)   { flags |= O_RDWR;   have_access = 1; }
        else if (strcmp(name, "nonblock") == 0) flags |= O_NONBLOCK;
        else return -1;
    }
    if (!have_access)
        flags |= O_RDWR; // sensible default for endpoint files and chardevs
    *out = flags;
    return 0;
}

// Reads are bounded; endpoint transfers are far below this.
#define READ_MAX_LEN (16u * 1024 * 1024)

// Shared read body: allocate, read (with the chosen locking strategy), shrink
// to what arrived. `blocking` selects begin/end_blocking (dirty scheduler, lock
// released across the syscall) vs a brief inline lock.
static ERL_NIF_TERM do_read(ErlNifEnv *env, const ERL_NIF_TERM argv[], int blocking) {
    GadgetFd *r;
    unsigned long count;
    if (!enif_get_resource(env, argv[0], gadget_fd_type, (void **)&r))
        return enif_make_badarg(env);
    if (!enif_get_ulong(env, argv[1], &count))
        return enif_make_badarg(env);
    if (count > READ_MAX_LEN)
        return enif_make_badarg(env);

    ErlNifBinary bin;
    if (!enif_alloc_binary((size_t)count, &bin))
        return err_tuple(env, ENOMEM);

    ssize_t n;
    int e = 0;
    if (blocking) {
        int fd = begin_blocking(r);
        if (fd < 0) {
            enif_release_binary(&bin);
            return enif_make_tuple2(env, am_error, am_ebadf);
        }
        n = read(fd, bin.data, (size_t)count);
        e = errno;
        end_blocking(r);
    } else {
        enif_mutex_lock(r->lock);
        if (r->fd < 0 || r->closing) {
            enif_mutex_unlock(r->lock);
            enif_release_binary(&bin);
            return enif_make_tuple2(env, am_error, am_ebadf);
        }
        n = read(r->fd, bin.data, (size_t)count);
        e = errno;
        enif_mutex_unlock(r->lock);
    }

    if (n < 0) {
        enif_release_binary(&bin);
        return err_tuple(env, e);
    }
    if ((size_t)n != bin.size) {
        if (!enif_realloc_binary(&bin, (size_t)n)) {
            enif_release_binary(&bin);
            return err_tuple(env, ENOMEM);
        }
    }
    return enif_make_tuple2(env, am_ok, enif_make_binary(env, &bin));
}

// Shared write body, same locking split as do_read.
static ERL_NIF_TERM do_write(ErlNifEnv *env, const ERL_NIF_TERM argv[], int blocking) {
    GadgetFd *r;
    ErlNifBinary data;
    if (!enif_get_resource(env, argv[0], gadget_fd_type, (void **)&r))
        return enif_make_badarg(env);
    if (!enif_inspect_iolist_as_binary(env, argv[1], &data))
        return enif_make_badarg(env);

    ssize_t n;
    int e = 0;
    if (blocking) {
        int fd = begin_blocking(r);
        if (fd < 0)
            return enif_make_tuple2(env, am_error, am_ebadf);
        n = write(fd, data.data, data.size);
        e = errno;
        end_blocking(r);
    } else {
        enif_mutex_lock(r->lock);
        if (r->fd < 0 || r->closing) {
            enif_mutex_unlock(r->lock);
            return enif_make_tuple2(env, am_error, am_ebadf);
        }
        n = write(r->fd, data.data, data.size);
        e = errno;
        enif_mutex_unlock(r->lock);
    }

    if (n < 0)
        return err_tuple(env, e);
    return enif_make_tuple2(env, am_ok, enif_make_ulong(env, (unsigned long)n));
}

// ---- NIFs ----------------------------------------------------------------

// open(path :: binary, flags :: [atom]) -> {:ok, handle} | {:error, atom}
static ERL_NIF_TERM nif_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    ErlNifBinary path;
    if (!enif_inspect_binary(env, argv[0], &path) &&
        !enif_inspect_iolist_as_binary(env, argv[0], &path))
        return enif_make_badarg(env);
    if (path.size == 0 || path.size > 4095)
        return enif_make_badarg(env);

    int flags;
    if (parse_open_flags(env, argv[1], &flags) != 0)
        return enif_make_badarg(env);

    // NUL-terminate the path.
    char cpath[4096];
    memcpy(cpath, path.data, path.size);
    cpath[path.size] = '\0';

    int fd = open(cpath, flags);
    if (fd < 0)
        return err_tuple(env, errno);

    GadgetFd *r = enif_alloc_resource(gadget_fd_type, sizeof(GadgetFd));
    if (!r) {
        close(fd);
        return err_tuple(env, ENOMEM);
    }
    r->fd = fd;
    r->select_active = 0;
    r->closing = 0;
    r->busy = 0;
    r->lock = enif_mutex_create("bodge_usb_gadget_fd");
    if (!r->lock) {
        close(fd);
        r->fd = -1;
        enif_release_resource(r);
        return err_tuple(env, ENOMEM);
    }
    ERL_NIF_TERM term = enif_make_resource(env, r);
    enif_release_resource(r); // the term now owns the only reference
    return enif_make_tuple2(env, am_ok, term);
}

// close(handle) -> :ok   (idempotent)
//
// If the fd was ever armed for select, tear it down via ERL_NIF_SELECT_STOP so
// the fd is removed from the poller before close() -- an in-flight select over a
// closed fd is a bug class. The actual close happens in the stop callback.
// Otherwise close inline, deferring to any blocked read/write in flight.
static ERL_NIF_TERM nif_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GadgetFd *r;
    if (!enif_get_resource(env, argv[0], gadget_fd_type, (void **)&r))
        return enif_make_badarg(env);

    enif_mutex_lock(r->lock);
    if (r->closing || r->fd < 0) {
        enif_mutex_unlock(r->lock);
        return am_ok;
    }
    r->closing = 1;
    int select_active = r->select_active;
    int fd = r->fd;

    if (select_active) {
        // Release the lock BEFORE enif_select(STOP): the stop callback may run
        // synchronously in this thread and it takes r->lock -- holding it here
        // would deadlock. `closing` already blocks any concurrent re-entry.
        enif_mutex_unlock(r->lock);
        enif_select(env, (ErlNifEvent)fd, ERL_NIF_SELECT_STOP, r, NULL,
                    enif_make_atom(env, "undefined"));
        return am_ok;
    }

    try_close_locked(r);
    enif_mutex_unlock(r->lock);
    return am_ok;
}

static ERL_NIF_TERM nif_read(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    return do_read(env, argv, 0);
}

static ERL_NIF_TERM nif_write(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    return do_write(env, argv, 0);
}

static ERL_NIF_TERM nif_read_blocking(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    return do_read(env, argv, 1);
}

static ERL_NIF_TERM nif_write_blocking(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    return do_write(env, argv, 1);
}

// select_read(handle, ref) -> :ok | {:error, atom}
// Arm enif_select for read-readiness (POLLIN); used for ep0 events. The caller
// gets `{:select, handle, ref, :ready_input}`.
static ERL_NIF_TERM nif_select_read(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GadgetFd *r;
    if (!enif_get_resource(env, argv[0], gadget_fd_type, (void **)&r))
        return enif_make_badarg(env);

    enif_mutex_lock(r->lock);
    if (r->fd < 0 || r->closing) {
        enif_mutex_unlock(r->lock);
        return enif_make_tuple2(env, am_error, am_ebadf);
    }
    int rc = enif_select(env, (ErlNifEvent)r->fd, ERL_NIF_SELECT_READ, r, NULL, argv[1]);
    if (rc >= 0)
        r->select_active = 1;
    enif_mutex_unlock(r->lock);

    if (rc < 0)
        return enif_make_tuple2(env, am_error, enif_make_atom(env, "eselect"));
    return am_ok;
}

// fileno(handle) -> integer | {:error, :ebadf}   (test aid; verifies no leak)
static ERL_NIF_TERM nif_fileno(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
    (void)argc;
    GadgetFd *r;
    if (!enif_get_resource(env, argv[0], gadget_fd_type, (void **)&r))
        return enif_make_badarg(env);
    int fd;
    enif_mutex_lock(r->lock);
    fd = r->closing ? -1 : r->fd;
    enif_mutex_unlock(r->lock);
    if (fd < 0)
        return enif_make_tuple2(env, am_error, am_ebadf);
    return enif_make_int(env, fd);
}

// ---- load ----------------------------------------------------------------

static int load(ErlNifEnv *env, void **priv_data, ERL_NIF_TERM load_info) {
    (void)priv_data;
    (void)load_info;
    ErlNifResourceFlags tried;
    ErlNifResourceTypeInit init = {0};
    init.dtor = gadget_fd_dtor;
    init.stop = gadget_fd_stop; // needed for enif_select teardown
    gadget_fd_type = enif_open_resource_type_x(env, "bodge_usb_gadget_fd", &init,
                                               ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER,
                                               &tried);
    if (!gadget_fd_type)
        return -1;

    am_ok = mk_atom(env, "ok");
    am_error = mk_atom(env, "error");
    am_ebadf = mk_atom(env, "ebadf");
    return 0;
}

// Re-take the resource type (and re-init atoms) so hot code upgrade works.
static int upgrade(ErlNifEnv *env, void **priv_data, void **old_priv_data,
                   ERL_NIF_TERM load_info) {
    (void)old_priv_data;
    return load(env, priv_data, load_info);
}

static ErlNifFunc nif_funcs[] = {
    {"open", 2, nif_open, 0},
    {"close", 1, nif_close, 0},
    {"read", 2, nif_read, 0},
    {"write", 2, nif_write, 0},
    // Blocking peer-driven I/O (FunctionFS endpoint files): dirty I/O with the
    // fd lock released across the syscall (begin/end_blocking).
    {"read_blocking", 2, nif_read_blocking, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"write_blocking", 2, nif_write_blocking, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"select_read", 2, nif_select_read, 0},
    {"fileno", 1, nif_fileno, 0},
};

ERL_NIF_INIT(Elixir.BodgeUSBGadget.Nif, nif_funcs, load, NULL, upgrade, NULL)
