#include "loader/registry.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"

/* ------------------------------------------------------------------ */
/* Internal types                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    void               *dlhandle;
    const BetlProvider *provider;
    char               *path;        /* strdup of the path we were given */
} LoadedProvider;

typedef struct {
    const char             *name;        /* points into provider's static string */
    const BetlComponentDef *def;         /* points into provider's components[]  */
    size_t                  provider_ix; /* index into providers[] */
} ComponentEntry;

typedef struct {
    const char           *lang;         /* points into engine's static string */
    const BetlExprEngine *engine;       /* points into provider->expr_engine  */
    size_t                provider_ix;
} ExprEntry;

struct BetlRegistry {
    LoadedProvider  *providers;
    size_t           providers_count;
    size_t           providers_cap;

    ComponentEntry  *components;
    size_t           components_count;
    size_t           components_cap;

    ExprEntry       *exprs;
    size_t           exprs_count;
    size_t           exprs_cap;

    char             err[512];
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void set_err(BetlRegistry *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof r->err, fmt, ap);
    va_end(ap);
}

static int grow_providers(BetlRegistry *r) {
    size_t new_cap = r->providers_cap ? r->providers_cap * 2 : 4;
    LoadedProvider *p = realloc(r->providers, new_cap * sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    r->providers     = p;
    r->providers_cap = new_cap;
    return BETL_OK;
}

static int grow_components(BetlRegistry *r) {
    size_t new_cap = r->components_cap ? r->components_cap * 2 : 16;
    ComponentEntry *c = realloc(r->components, new_cap * sizeof *c);
    if (!c) return BETL_ERR_INTERNAL;
    r->components     = c;
    r->components_cap = new_cap;
    return BETL_OK;
}

static int grow_exprs(BetlRegistry *r) {
    size_t new_cap = r->exprs_cap ? r->exprs_cap * 2 : 4;
    ExprEntry *e = realloc(r->exprs, new_cap * sizeof *e);
    if (!e) return BETL_ERR_INTERNAL;
    r->exprs     = e;
    r->exprs_cap = new_cap;
    return BETL_OK;
}

static const ComponentEntry *find_entry(const BetlRegistry *r,
                                        const char *name) {
    for (size_t i = 0; i < r->components_count; ++i) {
        if (strcmp(r->components[i].name, name) == 0) {
            return &r->components[i];
        }
    }
    return NULL;
}

static const ExprEntry *find_expr_entry(const BetlRegistry *r,
                                        const char *lang) {
    for (size_t i = 0; i < r->exprs_count; ++i) {
        if (strcmp(r->exprs[i].lang, lang) == 0) return &r->exprs[i];
    }
    return NULL;
}

/* dlsym returns void*; ISO C forbids casting object pointers to function
 * pointers. POSIX allows it for dlsym specifically; suppress -Wpedantic
 * around the one cast. */
typedef const BetlProvider *(*BetlEntryFn)(void);

static BetlEntryFn cast_entry(void *sym) {
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
    BetlEntryFn fn = (BetlEntryFn)sym;
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    return fn;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

BetlRegistry *betl_registry_create(void) {
    BetlRegistry *r = calloc(1, sizeof *r);
    return r;  /* NULL on OOM is fine */
}

void betl_registry_destroy(BetlRegistry *r) {
    if (!r) return;
    /* Tear down providers in reverse load order. */
    for (size_t i = r->providers_count; i-- > 0; ) {
        LoadedProvider *lp = &r->providers[i];
        if (lp->provider && lp->provider->provider_shutdown) {
            lp->provider->provider_shutdown();
        }
        if (lp->dlhandle) dlclose(lp->dlhandle);
        free(lp->path);
    }
    free(r->providers);
    free(r->components);
    free(r->exprs);
    free(r);
}

/* Validate a provider and commit it to the registry. `dlhandle` may be
 * NULL for in-process providers. `label` is used in diagnostics. On
 * failure: the dlhandle (if any) is dlclose'd and the registry is
 * unchanged. */
static int register_validated(BetlRegistry *r,
                              void *dlhandle,
                              const BetlProvider *prov,
                              const char *label) {
    if (!prov) {
        set_err(r, "provider entry returned NULL for %s", label);
        if (dlhandle) dlclose(dlhandle);
        return BETL_ERR_INVALID;
    }
    if (prov->abi_version != BETL_ABI_VERSION) {
        set_err(r, "ABI mismatch in %s: provider=%u host=%u",
                label, (unsigned)prov->abi_version, (unsigned)BETL_ABI_VERSION);
        if (dlhandle) dlclose(dlhandle);
        return BETL_ERR_UNSUPPORTED;
    }
    if (!prov->name || !prov->version || !prov->license) {
        set_err(r, "provider in %s missing name/version/license", label);
        if (dlhandle) dlclose(dlhandle);
        return BETL_ERR_INVALID;
    }
    if (prov->component_count > 0 && !prov->components) {
        set_err(r, "provider %s has component_count=%zu but components=NULL",
                prov->name, prov->component_count);
        if (dlhandle) dlclose(dlhandle);
        return BETL_ERR_INVALID;
    }

    /* Pre-flight: check every component name is non-NULL and not already
     * registered. Doing this BEFORE we mutate the registry keeps the
     * "no partial registration on failure" guarantee. */
    for (size_t i = 0; i < prov->component_count; ++i) {
        const BetlComponentDef *cd = &prov->components[i];
        if (!cd->name) {
            set_err(r, "provider %s component[%zu] has NULL name",
                    prov->name, i);
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INVALID;
        }
        if (find_entry(r, cd->name)) {
            set_err(r, "component name collision: %s (provider %s)",
                    cd->name, prov->name);
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INVALID;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(prov->components[j].name, cd->name) == 0) {
                set_err(r, "provider %s declares %s twice",
                        prov->name, cd->name);
                if (dlhandle) dlclose(dlhandle);
                return BETL_ERR_INVALID;
            }
        }
    }

    /* Pre-flight: validate the optional expression engine (lang must be
     * non-empty, vtable methods all set, no lang collision). */
    if (prov->expr_engine) {
        const BetlExprEngine *e = prov->expr_engine;
        if (!e->lang || !e->lang[0]) {
            set_err(r, "provider %s expr_engine has empty lang", prov->name);
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INVALID;
        }
        if (!e->compile || !e->evaluate || !e->release) {
            set_err(r, "provider %s expr_engine '%s' has incomplete vtable",
                    prov->name, e->lang);
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INVALID;
        }
        if (find_expr_entry(r, e->lang)) {
            set_err(r, "expr engine lang collision: %s (provider %s)",
                    e->lang, prov->name);
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INVALID;
        }
    }

    if (prov->provider_init) {
        int rc = prov->provider_init();
        if (rc != BETL_OK) {
            set_err(r, "provider %s provider_init returned %d", prov->name, rc);
            if (dlhandle) dlclose(dlhandle);
            return rc;
        }
    }

    if (r->providers_count == r->providers_cap) {
        if (grow_providers(r) != BETL_OK) {
            set_err(r, "out of memory growing providers table");
            if (prov->provider_shutdown) prov->provider_shutdown();
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INTERNAL;
        }
    }
    while (r->components_count + prov->component_count > r->components_cap) {
        if (grow_components(r) != BETL_OK) {
            set_err(r, "out of memory growing components table");
            if (prov->provider_shutdown) prov->provider_shutdown();
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INTERNAL;
        }
    }
    if (prov->expr_engine && r->exprs_count == r->exprs_cap) {
        if (grow_exprs(r) != BETL_OK) {
            set_err(r, "out of memory growing expr table");
            if (prov->provider_shutdown) prov->provider_shutdown();
            if (dlhandle) dlclose(dlhandle);
            return BETL_ERR_INTERNAL;
        }
    }

    char *label_copy = strdup(label);
    if (!label_copy) {
        set_err(r, "out of memory copying label");
        if (prov->provider_shutdown) prov->provider_shutdown();
        if (dlhandle) dlclose(dlhandle);
        return BETL_ERR_INTERNAL;
    }

    size_t pix = r->providers_count;
    r->providers[pix].dlhandle = dlhandle;
    r->providers[pix].provider = prov;
    r->providers[pix].path     = label_copy;
    r->providers_count++;

    for (size_t i = 0; i < prov->component_count; ++i) {
        ComponentEntry *e = &r->components[r->components_count++];
        e->name        = prov->components[i].name;
        e->def         = &prov->components[i];
        e->provider_ix = pix;
    }
    if (prov->expr_engine) {
        ExprEntry *e = &r->exprs[r->exprs_count++];
        e->lang        = prov->expr_engine->lang;
        e->engine      = prov->expr_engine;
        e->provider_ix = pix;
    }

    r->err[0] = '\0';
    return BETL_OK;
}

int betl_registry_load(BetlRegistry *r, const char *path) {
    if (!r || !path) return BETL_ERR_INVALID;

    /* Reject reload of the same path. We compare by path string, not
     * inode — symlinks resolving to the same file would be re-loaded.
     * Good enough for v0.1. */
    for (size_t i = 0; i < r->providers_count; ++i) {
        if (strcmp(r->providers[i].path, path) == 0) {
            set_err(r, "provider already loaded: %s", path);
            return BETL_ERR_INVALID;
        }
    }

    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *e = dlerror();
        set_err(r, "dlopen(%s) failed: %s", path, e ? e : "(unknown)");
        return BETL_ERR_IO;
    }

    dlerror();
    void *sym = dlsym(handle, "betl_provider_entry");
    const char *dle = dlerror();
    if (!sym || dle) {
        set_err(r, "dlsym(betl_provider_entry) in %s: %s",
                path, dle ? dle : "symbol is NULL");
        dlclose(handle);
        return BETL_ERR_IO;
    }

    BetlEntryFn entry_fn = cast_entry(sym);
    const BetlProvider *prov = entry_fn();
    return register_validated(r, handle, prov, path);
}

int betl_registry_register(BetlRegistry *r,
                           const BetlProvider *prov,
                           const char *label) {
    if (!r || !prov) return BETL_ERR_INVALID;
    if (!label) label = "<builtin>";
    /* Reject double-registration by label. */
    for (size_t i = 0; i < r->providers_count; ++i) {
        if (strcmp(r->providers[i].path, label) == 0) {
            set_err(r, "provider already registered: %s", label);
            return BETL_ERR_INVALID;
        }
    }
    return register_validated(r, NULL, prov, label);
}

const char *betl_registry_last_error(const BetlRegistry *r) {
    return r ? r->err : "";
}

const BetlComponentDef *betl_registry_find(const BetlRegistry *r,
                                           const char *component_name) {
    if (!r || !component_name) return NULL;
    const ComponentEntry *e = find_entry(r, component_name);
    return e ? e->def : NULL;
}

size_t betl_registry_provider_count(const BetlRegistry *r) {
    return r ? r->providers_count : 0;
}

size_t betl_registry_component_count(const BetlRegistry *r) {
    return r ? r->components_count : 0;
}

const BetlExprEngine *betl_registry_find_expr(const BetlRegistry *r,
                                              const char *lang) {
    if (!r || !lang) return NULL;
    const ExprEntry *e = find_expr_entry(r, lang);
    return e ? e->engine : NULL;
}

size_t betl_registry_expr_count(const BetlRegistry *r) {
    return r ? r->exprs_count : 0;
}
