# mcp-glib

This library currently targets MCP protocol version `2025-11-25` and owns:

- strict JSON-RPC parsing, validation, and errors;
- MCP initialization, version negotiation, and session state;
- built-in ping and cancellation handling;
- registered synchronous and asynchronous server methods;
- newline-delimited stdio connections supplied as `GInputStream` instances;
- a custom transport exchange API for embedding other transports.

Public APIs are grouped by responsibility in `mcp-server.h`, `mcp-session.h`,
`mcp-call.h`, and `mcp-transport.h`. `mcp-glib.h` is only an umbrella header.

JSON-RPC batches are rejected, as the protocol removed them in `2025-06-18`.
Concurrency comes from sending independent messages and correlating them by id,
which the stdio transport supports: it reads the next message without waiting
for the previous one to complete.

A ping is answered whatever the session state, since a client is allowed to send
one before `initialize`. Every other request before `initialize` closes the
session.

Normative references:

- [Base protocol](https://modelcontextprotocol.io/specification/2025-11-25/basic)
- [Lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
- [JSON-RPC 2.0](https://www.jsonrpc.org/specification)
