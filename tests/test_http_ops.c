/* http.get / http.post end-to-end against a local Python HTTP server.
 *
 * Spawns a tiny inline Python server that handles:
 *   GET  /hello       → 200 body "hello, betl"
 *   GET  /404         → 404
 *   POST /echo        → 200 body = request body verbatim
 *
 * Then drives:
 *   1. http.get /hello → save_to → assert file contents
 *   2. http.get /404   → run rc != OK, error mentions "HTTP 404"
 *   3. http.post /echo body="ping" → assert response equals "ping"
 *
 * Doesn't try to test TLS or auth — those would need a real cert /
 * credential setup. The smoke covers the libcurl wire happy paths.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static const char *PYTHON_SERVER =
    "import sys, http.server, socketserver\n"
    "class H(http.server.BaseHTTPRequestHandler):\n"
    "    def log_message(self, *a, **kw): pass\n"
    "    def do_GET(self):\n"
    "        if self.path == '/hello':\n"
    "            body = b'hello, betl'\n"
    "            self.send_response(200)\n"
    "            self.send_header('Content-Length', str(len(body)))\n"
    "            self.end_headers(); self.wfile.write(body)\n"
    "        else:\n"
    "            self.send_response(404); self.end_headers()\n"
    "            self.wfile.write(b'not found')\n"
    "    def do_POST(self):\n"
    "        n = int(self.headers.get('Content-Length','0'))\n"
    "        body = self.rfile.read(n)\n"
    "        self.send_response(200)\n"
    "        self.send_header('Content-Length', str(len(body)))\n"
    "        self.end_headers(); self.wfile.write(body)\n"
    "port = int(sys.argv[1])\n"
    "with socketserver.TCPServer(('127.0.0.1', port), H) as srv:\n"
    "    print('READY', flush=True)\n"
    "    srv.serve_forever()\n";

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int run_task(BetlRegistry *r, BetlContext *ctx,
                    const char *type, const char *cfg) {
    const BetlComponentDef *cd = betl_registry_find(r, type);
    if (!cd) return -1;
    void *state = NULL;
    int rc = cd->init(ctx, cfg, &state);
    if (rc != BETL_OK) return rc;
    rc = cd->task_run(state);
    cd->destroy(state);
    return rc;
}

int main(void) {
    /* 1. Stash the server script to a temp file. */
    char script_path[64];
    snprintf(script_path, sizeof script_path,
             "/tmp/betl-http-server-%d.py", (int)getpid());
    FILE *f = fopen(script_path, "w");
    if (!f || fwrite(PYTHON_SERVER, 1, strlen(PYTHON_SERVER), f)
              != strlen(PYTHON_SERVER)) {
        fprintf(stderr, "couldn't write server script\n");
        if (f) fclose(f);
        return 1;
    }
    fclose(f);

    /* 2. Pick a port. Most tooling uses ephemeral allocation; here
     * we hardcode a high port with PID modulo to dodge collisions
     * during parallel test runs. */
    int port = 18000 + ((int)getpid() % 1000);
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);
    char base_url[64];
    snprintf(base_url, sizeof base_url, "http://127.0.0.1:%d", port);

    /* 3. Fork the python server. The child's stdout is piped so we
     * can wait for the "READY" line before issuing requests. */
    int pipefd[2];
    if (pipe(pipefd) != 0) { unlink(script_path); return 1; }
    pid_t pid = fork();
    if (pid < 0) { unlink(script_path); return 1; }
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        setpgid(0, 0);
        execlp("python3", "python3", "-u", script_path, port_str, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    /* Wait for "READY\n" or give up after 5s. */
    int ready = 0;
    for (int tries = 0; tries < 50 && !ready; ++tries) {
        char buf[16] = {0};
        ssize_t n = read(pipefd[0], buf, sizeof buf - 1);
        if (n > 0 && strstr(buf, "READY")) { ready = 1; break; }
        struct timespec ts = { 0, 100 * 1000 * 1000 };  /* 100ms */
        nanosleep(&ts, NULL);
    }
    close(pipefd[0]);
    if (!ready) {
        fprintf(stderr, "[skip] server did not become ready\n");
        kill(-pid, SIGTERM); waitpid(pid, NULL, 0);
        unlink(script_path);
        return 77;
    }

    /* 4. Drive the http components. */
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    CHECK(betl_register_builtins(reg) == BETL_OK);

    char resp_path[64];
    snprintf(resp_path, sizeof resp_path,
             "/tmp/betl-http-resp-%d.txt", (int)getpid());

    /* --- 1: http.get /hello captures the body ----------------- */
    {
        unlink(resp_path);
        char cfg[256];
        snprintf(cfg, sizeof cfg,
            "{\"url\":\"%s/hello\",\"save_to\":\"%s\",\"timeout\":\"5s\"}",
            base_url, resp_path);
        CHECK(run_task(reg, ctx, "http.get", cfg) == BETL_OK);
        char *body = read_file(resp_path);
        CHECK(body && strcmp(body, "hello, betl") == 0);
        free(body);
    }

    /* --- 2: http.get /404 fails with status code in the error -- */
    {
        unlink(resp_path);
        char cfg[256];
        snprintf(cfg, sizeof cfg,
            "{\"url\":\"%s/404\",\"save_to\":\"%s\",\"timeout\":\"5s\"}",
            base_url, resp_path);
        int rc = run_task(reg, ctx, "http.get", cfg);
        CHECK(rc != BETL_OK);
        const char *e = betl_context_last_error(ctx);
        CHECK(e && strstr(e, "HTTP 404") != NULL);
        /* Failed-fetch path also unlinks the partial response. */
        struct stat st;
        CHECK(stat(resp_path, &st) != 0);
    }

    /* --- 3: http.post /echo round-trips the body --------------- */
    {
        unlink(resp_path);
        char cfg[256];
        snprintf(cfg, sizeof cfg,
            "{\"url\":\"%s/echo\",\"save_to\":\"%s\","
             "\"body\":\"ping-pong\",\"timeout\":\"5s\"}",
            base_url, resp_path);
        CHECK(run_task(reg, ctx, "http.post", cfg) == BETL_OK);
        char *body = read_file(resp_path);
        CHECK(body && strcmp(body, "ping-pong") == 0);
        free(body);
    }

    unlink(resp_path);
    unlink(script_path);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    /* 5. Stop the server. SIGTERM the process group so the python
     * child + its socketserver thread both exit. */
    kill(-pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: http_ops integration test passed\n");
    return 0;
}
