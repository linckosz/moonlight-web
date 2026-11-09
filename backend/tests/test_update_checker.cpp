/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "test_framework.h"
#include "network/UpdateChecker.h"

#include <QDateTime>

void run_update_checker_tests()
{
    SECTION("UpdateChecker: a fresh look at most once per floor, for every client");
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const int floor = UpdateChecker::kFreshFloorSeconds;
        CHECK(floor == 5 * 60);
        // Never checked: due.
        CHECK(UpdateChecker::freshDue(QDateTime(), now));
        // Just checked, or under the floor: the cache stands.
        CHECK(!UpdateChecker::freshDue(now, now));
        CHECK(!UpdateChecker::freshDue(now.addSecs(-(floor - 1)), now));
        // At the floor and past it: due again.
        CHECK(UpdateChecker::freshDue(now.addSecs(-floor), now));
        CHECK(UpdateChecker::freshDue(now.addSecs(-6 * 3600), now));
        // A clock set back: due, rather than waiting out the difference.
        CHECK(UpdateChecker::freshDue(now.addSecs(120), now));
    }

    SECTION("UpdateChecker: nothing to wait for where nothing may be asked");
    {
        // Opted out: no request leaves, so a fresh ask has no check to wait on
        // and the cache answers at once.
        const QByteArray before = qgetenv("MW_NO_TELEMETRY");
        qputenv("MW_NO_TELEMETRY", "1");
        UpdateChecker checker(QStringLiteral("0.4.2"), false);
        CHECK(!checker.requestFresh());
        CHECK(!checker.requestFresh());
        CHECK(checker.statusJson().value(QStringLiteral("update_available")).toBool() == false);
        if (before.isEmpty())
            qunsetenv("MW_NO_TELEMETRY");
        else
            qputenv("MW_NO_TELEMETRY", before);
    }
}
