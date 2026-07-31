# ===========================================================================
#  MoonlightWeb — Homebrew cask (TEMPLATE, not a valid cask as it stands).
#
#  @MW_VERSION@ / @MW_SHA256@ are substituted by the `homebrew` job in
#  .github/workflows/release.yml, which then pushes the result to
#  linckosz/homebrew-tap as Casks/moonlightweb.rb. Nothing here is used at
#  build time; edit this file, not the copy in the tap.
#
#      brew install --cask linckosz/tap/moonlightweb
#
#  Why the tap exists: the .pkg is not notarized (that needs a paid Apple
#  Developer ID — there is no free tier and no OSS programme), so a
#  double-clicked download is refused by Gatekeeper with "unidentified
#  developer". Homebrew hands the .pkg to installer(8) as root, and
#  installer(8) does not consult Gatekeeper — so this path installs with no
#  warning at all, quarantine attribute or not. Same trick as
#  website/install.sh, for people who already live in Homebrew.
#
#  Not homebrew-cask upstream: that requires ~75 stars / 30 forks
#  ("notability"). A personal tap needs no approval and installs identically.
# ===========================================================================
cask "moonlightweb" do
  version "@MW_VERSION@"
  sha256 "@MW_SHA256@"

  # `verified:` is required whenever the download host differs from homepage.
  url "https://github.com/linckosz/moonlight-web/releases/download/v#{version}/moonlightweb-#{version}-macos-arm64.pkg",
      verified: "github.com/linckosz/moonlight-web/"
  name "MoonlightWeb"
  desc "Stream your Sunshine games to any device with a browser"
  homepage "https://moonlightweb.top/"

  livecheck do
    url :url
    strategy :github_latest
  end

  # Apple Silicon only (the release job builds arm64), Monterey+ to match
  # LSMinimumSystemVersion in the bundle's Info.plist.
  depends_on arch: :arm64
  depends_on macos: ">= :monterey"

  pkg "moonlightweb-#{version}-macos-arm64.pkg"

  # Undoes what the pkg's postinstall sets up: the per-user LaunchAgent, the
  # running app, and the payload itself (pkgutil removes every file listed in
  # the receipt, /Applications/MoonlightWeb.app included).
  uninstall launchctl: "com.moonlightweb.agent",
            quit:      "com.moonlightweb.server",
            pkgutil:   "com.moonlightweb.server"

  # Settings, sessions, logs and the pairing state — kept on uninstall,
  # removed by `brew uninstall --zap`.
  zap trash: [
    "~/Library/Application Support/MoonlightWeb",
    "~/Library/LaunchAgents/com.moonlightweb.agent.plist",
  ]
end
