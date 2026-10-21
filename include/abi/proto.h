/* Conventions shared by every server protocol.
 *
 * A request's word 0 holds the operation in its low 8 bits; anything above
 * is operation-specific. A reply's word 0 is a result: >= 0 on success,
 * -ERR_* on failure.
 *
 * Clients reach a server through badged capabilities (badge >= 1, unique
 * per client). The root task keeps the only unbadged one, so badge 0 on a
 * request means "from the root task", and a server can trust it with
 * operations no client may perform. */
#pragma once

#define PROTO_OP(w0)      ((w0) & 0xff)
#define PROTO_SHUTDOWN    0xff   /* exit cleanly; only accepted from badge 0 */