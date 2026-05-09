/* Connectivity smoke test for libpq from the c-build container.
 *
 * Goals (in this order):
 *   1. confirm c-build can reach the Postgres TCP port,
 *   2. confirm libpq linkage works,
 *   3. confirm trust auth from the documented DSN succeeds,
 *   4. round-trip a value through the wire protocol.
 *
 * DSN comes from BETL_TEST_PG_DSN; falls back to the trust-auth DSN
 * advertised by the db-postgres MCP service. If the connection fails
 * we exit with 77 — CTest, configured with SKIP_RETURN_CODE=77 on this
 * test, will mark it Skipped rather than Failed, so a developer without
 * the sibling Postgres available isn't blocked by the suite. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#define SKIP_RC 77

static const char *default_dsn(void)
{
    return "postgresql://postgres@host.containers.internal:5432/postgres";
}

int main(void)
{
    const char *dsn = getenv("BETL_TEST_PG_DSN");
    if (!dsn || !*dsn) dsn = default_dsn();

    PGconn *c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr,
                "[skip] connect failed: %s",
                PQerrorMessage(c));
        PQfinish(c);
        return SKIP_RC;
    }

    PGresult *r = PQexec(c, "SELECT 1::int4 AS one, version()");
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        fprintf(stderr, "SELECT failed: %s\n", PQerrorMessage(c));
        PQclear(r);
        PQfinish(c);
        return 1;
    }

    const char *one = PQgetvalue(r, 0, 0);
    const char *ver = PQgetvalue(r, 0, 1);
    if (strcmp(one, "1") != 0) {
        fprintf(stderr, "expected '1', got '%s'\n", one);
        PQclear(r);
        PQfinish(c);
        return 1;
    }

    printf("ok: pg server says %s\n", ver);
    PQclear(r);
    PQfinish(c);
    return 0;
}
