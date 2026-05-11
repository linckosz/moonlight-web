---
name: HTTPS server debugging approach
type: feedback
description: When debugging HTTPS server connection issues, check QSslServer internals first — read the private header, not just the public API.
---

When debugging HTTPS server issues where connections fail after the first successful request,
first read the QSslServer private implementation header (`qsslserver_p.h`) rather than only
the public API. The private header reveals the `QHash<quintptr, SocketData>` tracking map
and the `totalPendingConnections()` override that are the root cause of the problem. These
details are not visible from QSslServer's public API alone.

**Why:** Qt's pattern `Q_DECLARE_PRIVATE` hides critical implementation details. The public
API shows signals like `startedEncryptionHandshake` but not the `SocketData` structure that
causes the descriptor-lifecycle bug.

**How to apply:** Server/socket debugging: always check `*_p.h` private headers in the Qt
installation (`/c/Qt/<version>/<arch>/include/QtNetwork/6.x.y/QtNetwork/private/`) before
attributing issues to application code.
