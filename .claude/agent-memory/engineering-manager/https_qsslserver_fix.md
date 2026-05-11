---
name: HTTPS QSslServer fix
type: project
description: Replacement of QSslServer with QTcpServer+manual QSslSocket to fix second-request timeout
---

QSslServer (Qt 6.11) maintains an internal `QHash<quintptr, SocketData>` keyed by socket
descriptor (`qsslserver_p.h`). When `QSslSocket::disconnectFromHost()` is called followed by
`deleteLater()`, the descriptor can be invalidated before the QSslServer's `destroyed` handler
runs, leaving zombie entries in the hash. This causes `totalPendingConnections()` to return
incorrect counts, eventually saturating the internal backlog and preventing new connections
from being accepted — manifesting as `ERR_TIMED_OUT` on the second HTTPS request.

**Why:** QSslServer is a relatively new class (Qt 6.2+) with complex internal socket tracking
that has edge cases in the socket descriptor lifecycle during disconnect/shutdown sequences.

**How to apply:** If similar HTTPS server issues arise (first request works, subsequent requests
time out), do NOT use QSslServer. Instead, use a plain QTcpServer + manually created QSslSocket
per connection, transferring the socket descriptor via `setSocketDescriptor(-1)` detachment.
