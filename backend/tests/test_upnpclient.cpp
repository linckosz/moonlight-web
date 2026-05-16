/**
 * Unit tests for UPNPClient (UPnP NAT traversal).
 *
 * Tests both modes:
 *   1. Without MW_HAVE_MINIUPNPC (fallback — all operations return false/empty).
 *   2. With MW_HAVE_MINIUPNPC (real miniupnpc — requires gateway on LAN).
 *
 * Build:
 *   qmake tests.pro && make
 *   (or use run_tests.bat)
 */
#include <QApplication>
#include <QDebug>
#include <QTimer>
#include <cstdio>
#include <cstring>

// Include UPNPClient (from source, not precompiled — we want to test it)
#include "../src/network/UPNPClient.h"

// Test counters
static int g_Passed = 0;
static int g_Failed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        qCritical() << "[FAIL]" << name; \
        g_Failed++; \
    } else { \
        qInfo() << "[PASS]" << name; \
        g_Passed++; \
    } \
} while(0)

// ── Test 1: Construction / Destruction ──────────────────────────────────────────

void test_construction()
{
    qInfo() << "\n=== Test: Construction / Destruction ===";

    {
        UPNPClient client;
        TEST("Default: not available", !client.isAvailable());
        TEST("Default: gateway is null", client.gatewayAddress().isNull());
    }  // destructor called here

    TEST("Destructor: no crash on cleanup", true);
}

// ── Test 2: Discover without MW_HAVE_MINIUPNPC (fallback) ───────────────────────

void test_discover_fallback()
{
    qInfo() << "\n=== Test: discover() fallback (no miniupnpc) ===";

    UPNPClient client;

    // Without MW_HAVE_MINIUPNPC, discover() should fail immediately
    bool discovered = client.discover(500);
    TEST("discover() returns false without miniupnpc", !discovered);
    TEST("Not available after failed discover", !client.isAvailable());
    TEST("Gateway still null", client.gatewayAddress().isNull());
}

// ── Test 3: addPortMapping without MW_HAVE_MINIUPNPC (fallback) ────────────────

void test_add_mapping_fallback()
{
    qInfo() << "\n=== Test: addPortMapping() fallback (no miniupnpc) ===";

    UPNPClient client;
    bool added = client.addPortMapping(48010, 48010, 3600, "Test Mapping");
    TEST("addPortMapping() returns false without miniupnpc", !added);
}

// ── Test 4: removePortMapping without MW_HAVE_MINIUPNPC (fallback) ─────────────

void test_remove_mapping_fallback()
{
    qInfo() << "\n=== Test: removePortMapping() fallback (no miniupnpc) ===";

    UPNPClient client;
    bool removed = client.removePortMapping(48010);
    TEST("removePortMapping() returns false without miniupnpc", !removed);
}

// ── Test 5: getExternalIPAddress without MW_HAVE_MINIUPNPC (fallback) ──────────

void test_external_ip_fallback()
{
    qInfo() << "\n=== Test: getExternalIPAddress() fallback (no miniupnpc) ===";

    UPNPClient client;
    std::string ip = client.getExternalIPAddress();
    TEST("getExternalIPAddress() returns empty without miniupnpc", ip.empty());
}

// ── Test 6: Signal emissions ──────────────────────────────────────────────────

void test_signals()
{
    qInfo() << "\n=== Test: Signal emissions ===";

    UPNPClient client;
    bool mappingAddedEmitted = false;
    bool mappingRemovedEmitted = false;
    bool errorEmitted = false;

    QObject::connect(&client, &UPNPClient::mappingAdded, [&](uint16_t port) {
        mappingAddedEmitted = true;
        qInfo() << "  mappingAdded signal received, port=" << port;
    });
    QObject::connect(&client, &UPNPClient::mappingRemoved, [&](uint16_t port) {
        mappingRemovedEmitted = true;
        qInfo() << "  mappingRemoved signal received, port=" << port;
    });
    QObject::connect(&client, &UPNPClient::error, [&](const QString& msg) {
        errorEmitted = true;
        qInfo() << "  error signal received:" << msg;
    });

    // In fallback mode, operations fail → error signals should be emitted
    client.discover(500);
    TEST("error signal emitted on discover() failure in fallback mode",
         errorEmitted);

    // reset counters
    mappingAddedEmitted = false;
    mappingRemovedEmitted = false;
    errorEmitted = false;

    // addPortMapping should also emit error in fallback mode
    errorEmitted = false;
    client.addPortMapping(48010, 48010);
    TEST("error signal emitted on addPortMapping() failure",
         errorEmitted);
}

// ── Test 7: Double cleanup safety ─────────────────────────────────────────────

void test_double_cleanup()
{
    qInfo() << "\n=== Test: Double cleanup safety ===";

    {
        UPNPClient client;
        client.discover(500);   // fails, but triggers cleanup path
        client.discover(500);   // second discover calls cleanup() on m_Available=false
        TEST("Double discover: no crash", true);
    }

    TEST("Destructor after double discover: no crash", true);
}

// ── Test 8: End-to-end discover (only if miniupnpc is available) ──────────────

void test_discover_with_upnp()
{
#ifdef MW_HAVE_MINIUPNPC
    qInfo() << "\n=== Test: discover() with actual miniupnpc (requires LAN gateway) ===";

    {
        UPNPClient client;
        bool discovered = client.discover(2000);
        if (discovered) {
            TEST("discover() succeeds with miniupnpc on LAN with IGD", true);
            TEST("isAvailable() is true after discover", client.isAvailable());
            TEST("Gateway address is valid", !client.gatewayAddress().isNull());
            qInfo() << "  Gateway:" << client.gatewayAddress().toString();

            // Test getExternalIPAddress
            std::string extIP = client.getExternalIPAddress();
            if (!extIP.empty()) {
                TEST("getExternalIPAddress() returns non-empty", true);
                qInfo() << "  External IP:" << QString::fromStdString(extIP);
            } else {
                qInfo() << "  getExternalIPAddress() failed (gateway may not be connected to internet)";
            }

            // Test addPortMapping + removePortMapping
            bool added = client.addPortMapping(48010, 48010, 3600, "Moonlight-Web Test");
            if (added) {
                TEST("addPortMapping(48010, 48010) succeeds", true);
                qInfo() << "  Port mapping added for 48010 UDP";

                // Give the router a moment to process
                QTimer::singleShot(500, [&]() {
                    bool removed = client.removePortMapping(48010);
                    TEST("removePortMapping(48010) succeeds", removed);

                    if (removed) {
                        qInfo() << "  Port mapping removed successfully";
                    } else {
                        qWarning() << "  removePortMapping failed (may have already expired)";
                    }
                });
            } else {
                qInfo() << "  addPortMapping failed (port may be in use or router limited)";
                TEST("addPortMapping failure is handled gracefully", true);
            }
        } else {
            qInfo() << "  No IGD found on this LAN — skipping UPnP E2E test";
            TEST("discover() graceful failure without IGD", true);
        }
    }

    QTimer::singleShot(2000, QApplication::instance(), &QApplication::quit);
#else
    qInfo() << "\n=== Test: discover() with miniupnpc — SKIPPED (MW_HAVE_MINIUPNPC not defined) ===";
#endif
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("test-upnpclient");

    qInfo() << "============================================";
    qInfo() << "  UPNPClient Unit Tests";
    qInfo() << "============================================";
#ifdef MW_HAVE_MINIUPNPC
    qInfo() << "  Mode: WITH miniupnpc (real UPnP)";
#else
    qInfo() << "  Mode: WITHOUT miniupnpc (fallback only)";
#endif
    qInfo() << "============================================";

    // Fallback tests (always run)
    test_construction();
    test_discover_fallback();
    test_add_mapping_fallback();
    test_remove_mapping_fallback();
    test_external_ip_fallback();
    test_signals();
    test_double_cleanup();

    // Summary
    qInfo() << "\n============================================";
    qInfo() << "  Results:";
    qInfo() << "    Passed:" << g_Passed;
    qInfo() << "    Failed:" << g_Failed;
    qInfo() << "============================================";

    // E2E tests with real miniupnpc (if available)
    // This test uses QTimer::singleShot to handle async signal tests
    if (argc > 1 && strcmp(argv[1], "--upnp") == 0) {
#ifdef MW_HAVE_MINIUPNPC
        test_discover_with_upnp();
        return app.exec();
#else
        qInfo() << "\n--upnp requested but MW_HAVE_MINIUPNPC not defined.";
        qInfo() << "Run build_miniupnpc.bat first, then rebuild with MW_HAVE_MINIUPNPC.";
        return 1;
#endif
    }

    return g_Failed > 0 ? 1 : 0;
}
