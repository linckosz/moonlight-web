/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The account an installation was made by (common/OwnerAccount): a SID the
 * installer records, compared with the account behind a token. Nothing
 * recorded, or anything that is not a SID, and nobody is the owner.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "test_framework.h"
#include "common/OwnerAccount.h"

#include <QString>

#include <string>

void run_owner_account_tests()
{
    using namespace mw::owneraccount;
    SECTION("OwnerAccount: the product's record is the machine's, the installer's key");
    const Store product = machineStore();
    CHECK(product.hive == Store::Hive::Machine);
    CHECK(product.key.startsWith(QStringLiteral("SOFTWARE\\")));
    CHECK(product.key.endsWith(QStringLiteral("\\Setup")));
    // A store is the machine's unless it says otherwise
    CHECK(Store{}.hive == Store::Hive::Machine);

    SECTION("OwnerAccount: two accounts are one when both are SIDs, and equal");
    const QString system = QStringLiteral("S-1-5-18");
#ifdef _WIN32
    HANDLE token = nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);
    const QString me = accountOf(token);
    CHECK(me.startsWith(QStringLiteral("S-1-")));
    CHECK(accountOf(nullptr).isEmpty());

    CHECK(same(me, me));
    CHECK(same(system, system));
    CHECK(!same(me, system));
    CHECK(!same(system, me));
    // Anything missing is « no »
    CHECK(!same(me, QString()));
    CHECK(!same(QString(), me));
    CHECK(!same(QString(), QString()));
    // A name is not a SID, and neither is a SID with something after it
    CHECK(!same(QStringLiteral("bruno"), QStringLiteral("bruno")));
    CHECK(!same(me + QStringLiteral(" x"), me + QStringLiteral(" x")));
    CHECK(!same(me, me + QStringLiteral("-1")));

    SECTION("OwnerAccount: the system's own accounts are nobody's");
    CHECK(isServiceAccount(system));
    CHECK(isServiceAccount(QStringLiteral("S-1-5-19")));
    CHECK(isServiceAccount(QStringLiteral("S-1-5-20")));
    CHECK(!isServiceAccount(me));
    // The administrators' group is not one of them, nor a name, nor nothing
    CHECK(!isServiceAccount(QStringLiteral("S-1-5-32-544")));
    CHECK(!isServiceAccount(QStringLiteral("SYSTEM")));
    CHECK(!isServiceAccount(QString()));

    SECTION("OwnerAccount: the owner is the recorded account, and nobody when none is");
    // In a key of the tests' own, under this user's hive: the installer's
    // place is the machine's, which the tests may not write. The store is
    // handed to each call; nothing in the product is switched over to it
    const wchar_t parentW[] = L"Software\\MoonlightWebTests";
    const QString key = QString::fromWCharArray(parentW) +
                        QStringLiteral("\\OwnerAccount-%1").arg(GetCurrentProcessId());
    const std::wstring keyW = key.toStdWString();
    const Store store{Store::Hive::CurrentUser, key};
    const auto write = [&keyW](const wchar_t* text) {
        return RegSetKeyValueW(HKEY_CURRENT_USER, keyW.c_str(), L"OwnerSid", REG_SZ, text,
                               static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };
    // Nothing recorded: nobody is the owner, this account included
    forget(store);
    CHECK(recorded(store).isEmpty());
    CHECK(!isOwner(store, token));
    // Recorded by this process, as the installer has it done: this account is
    QString why;
    CHECK(record(store, &why));
    CHECK(recorded(store) == me);
    CHECK(isOwner(store, token));
    CHECK(!isOwner(store, nullptr));
    // Another account recorded: this one is not the owner
    CHECK(write(L"S-1-5-18"));
    CHECK(recorded(store) == system);
    CHECK(!isOwner(store, token));
    // A record that is no SID names nobody
    CHECK(write(L"bruno"));
    CHECK(!isOwner(store, token));
    CHECK(write(L""));
    CHECK(!isOwner(store, token));
    // Recorded again (an install over it, a repair): the new account
    CHECK(record(store, &why));
    CHECK(isOwner(store, token));
    // A record in one place says nothing of another: the same key name in a
    // store without a record names nobody
    const Store elsewhere{Store::Hive::CurrentUser, key + QStringLiteral("-none")};
    CHECK(recorded(elsewhere).isEmpty());
    CHECK(!isOwner(elsewhere, token));
    // A store without a key holds nothing and takes nothing
    const Store keyless{Store::Hive::CurrentUser, QString()};
    why.clear();
    CHECK(!record(keyless, &why));
    CHECK(!why.isEmpty());
    CHECK(recorded(keyless).isEmpty());
    CHECK(!isOwner(keyless, token));
    forget(keyless);
    // Forgotten (the uninstaller): nobody again
    forget(store);
    CHECK(recorded(store).isEmpty());
    CHECK(!isOwner(store, token));
    forget(store); // twice is nothing

    // The tests' key goes, and its parent with it when nothing else is under
    // it (a key that has subkeys is not deleted)
    RegDeleteKeyW(HKEY_CURRENT_USER, keyW.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, parentW);
    CloseHandle(token);
#else
    // Elsewhere nothing is recorded and nobody is the owner
    QString why;
    CHECK(!record(product, &why));
    CHECK(!why.isEmpty());
    CHECK(recorded(product).isEmpty());
    CHECK(accountOf(nullptr).isEmpty());
    CHECK(!same(system, system));
    CHECK(!isOwner(product, nullptr));
    CHECK(!isServiceAccount(system));
    forget(product);
#endif
}
