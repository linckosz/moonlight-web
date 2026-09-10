// ============================================================================
// The way out of a bench kiosk, for a hand on a mouse.
//
//   <script src="escape.js"></script>   -- that is the whole integration.
//
// The kiosk is a borderless --kiosk Chrome pinned HWND_TOPMOST: no title bar,
// no close button, and Alt+F4 closes whatever holds the FOCUS, which is behind
// the overlay. Ctrl+Alt+Shift+Q covers that from anywhere, but a shortcut only
// helps somebody who knows it, and somebody who has just sat down in front of
// a screen full of video does not. So: a close button, in the corner.
//
// The constraint that shapes every choice below is that THIS PAGE IS THE
// MEASUREMENT. Whatever it draws, the host encodes and the campaign scores. So
// the button is not on screen. It is armed only while the pointer is actually
// in its corner, and during a campaign the host pointer is parked motionless
// in the inert click target at 50% x 72% of the screen (run-browser.ps1) - far
// outside the corner, and not moving. Hovering is a deliberate act; a pass
// cannot perform it by accident.
//
// Two more consequences of the same rule:
//   - the corner is the TOP RIGHT, because the click-to-photon flag is painted
//     across 44%..56% x 0..5% - the top MIDDLE. A control over the flag would
//     be measured as part of it.
//   - no key handler, ever. A keystroke arriving from the stream must not be
//     able to stop a bench mid-pass. The mouse is safe here in a way the
//     keyboard is not: the injected pointer parks somewhere known and stays.
//
// window.close() genuinely works on these windows - verified on the bench, in
// --kiosk, where history.length is 1 and the tab was opened by the browser
// rather than by script. When it is ever refused, the fallback says so and
// names the shortcut instead of leaving somebody clicking a dead button.
// ============================================================================
(function () {
  "use strict";

  var CORNER_W = 200;    // px of the top-right corner that arms the button
  var CORNER_H = 160;
  var LINGER_MS = 2500;  // how long it stays up after the pointer leaves
  // The hint is gone before the first measured frame: the runner starts its
  // pass 3.5 s after this page opens, and cod.html starts playing at 3 s.
  var HINT_FADE_MS = 2400;
  var HINT_GONE_MS = 2900;

  var css = document.createElement("style");
  css.textContent = [
    "#mw-escape{position:fixed;top:16px;right:16px;width:44px;height:44px;z-index:2147483647;",
    "  display:grid;place-content:center;border-radius:50%;border:1px solid rgba(255,255,255,.35);",
    "  background:rgba(0,0,0,.66);color:#fff;font:22px/1 system-ui,sans-serif;cursor:pointer;",
    "  opacity:0;pointer-events:none;transition:opacity .18s;-webkit-user-select:none;user-select:none}",
    "#mw-escape:hover{background:rgba(0,0,0,.86);border-color:#fff}",
    "#mw-escape.on{opacity:1;pointer-events:auto}",
    "#mw-escape-tip{position:fixed;top:70px;right:16px;z-index:2147483647;color:#eee;",
    "  background:rgba(0,0,0,.72);border-radius:6px;padding:.4rem .7rem;",
    "  font:13px/1.4 system-ui,sans-serif;opacity:0;pointer-events:none;transition:opacity .18s}",
    "#mw-escape-tip.on{opacity:1}",
    // The pages hide the cursor so it is not in the captured picture. While the
    // button is armed it has to come back - you cannot aim at what you cannot
    // see - and that is exactly the moment the picture is not being measured.
    "html.mw-escape-armed,html.mw-escape-armed body{cursor:default!important}",
    "#mw-escape-hint{position:fixed;left:50%;bottom:8%;transform:translateX(-50%);z-index:2147483647;",
    "  color:#ddd;background:rgba(0,0,0,.72);border:1px solid #444;border-radius:8px;",
    "  padding:.9rem 1.4rem;font:15px/1.5 system-ui,sans-serif;text-align:center;transition:opacity .4s}",
    "#mw-escape-hint b{color:#fff;font-weight:600}"
  ].join("\n");
  document.head.appendChild(css);

  var btn = document.createElement("div");
  btn.id = "mw-escape";
  btn.setAttribute("role", "button");
  btn.setAttribute("aria-label", "close the bench kiosk");
  btn.textContent = "✕";

  var tip = document.createElement("div");
  tip.id = "mw-escape-tip";
  tip.textContent = "close this window";

  var hint = document.createElement("div");
  hint.id = "mw-escape-hint";
  hint.innerHTML =
    "bench content — close it with the <b>✕</b> in the top right corner," +
    "<br>or <b>Ctrl+Alt+Shift+Q</b> from anywhere";

  function attach() {
    document.body.appendChild(btn);
    document.body.appendChild(tip);
    document.body.appendChild(hint);
    setTimeout(function () { hint.style.opacity = "0"; }, HINT_FADE_MS);
    setTimeout(function () { if (hint.parentNode) hint.remove(); }, HINT_GONE_MS);
  }
  if (document.body) { attach(); }
  else { document.addEventListener("DOMContentLoaded", attach); }

  var timer = 0;
  function arm(on) {
    btn.classList.toggle("on", on);
    tip.classList.toggle("on", on);
    document.documentElement.classList.toggle("mw-escape-armed", on);
  }

  window.addEventListener("mousemove", function (e) {
    var inCorner = e.clientX > window.innerWidth - CORNER_W && e.clientY < CORNER_H;
    if (inCorner) {
      if (timer) { clearTimeout(timer); timer = 0; }
      arm(true);
    } else if (!timer) {
      timer = setTimeout(function () { timer = 0; arm(false); }, LINGER_MS);
    }
  }, { passive: true });

  btn.addEventListener("click", function () {
    window.close();
    // Chrome refuses window.close() on a window it did not open by script. It
    // does allow it here, but a page that silently does nothing when clicked is
    // worse than no button at all - so say what to do instead.
    setTimeout(function () {
      tip.textContent = "this window refuses to close — press Ctrl+Alt+Shift+Q";
      tip.classList.add("on");
      arm(true);
    }, 500);
  });
})();
