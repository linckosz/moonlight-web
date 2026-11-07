/**
 * OthersOff — « Turn off the host's other screens », a box of the streaming
 * settings (issue #39).
 *
 * Two keys, both needed. The administrator allows it (Admin, native host,
 * Windows with "MoonlightWeb Virtual Display" installed), and each browser
 * chooses it for its own streams on that display. The choice is this
 * browser's alone: it rides every launch as `others_off`, and is never made
 * the server's default for the other browsers. A guest has no say: the server
 * ignores it for an invitation's stream.
 *
 * A browser that never chose sends nothing, and the server applies the PC's
 * inherited default: ticked on a PC whose admin box was ticked before the
 * choice moved to the viewer, so an update changes nothing there.
 *
 * The host's card says both (nativeDisplay.others_off): `allowed`, and
 * `inherited` (what a browser that never chose gets). The displays a stream
 * turned off come as dark cards of their own (App.isOff).
 */

/** The native host's others_off block, or null where there is none. */
function othersOffOf(host) {
    if (host?.backendType !== 'native') return null;
    const block = host?.nativeDisplay?.others_off;
    return block && typeof block === 'object' ? block : null;
}

/**
 * What Settings offers, from a host list (GET /api/hosts' `hosts`): null when
 * no host allows it (the box is not shown), else {inherited}.
 */
export function othersOffOffer(hosts) {
    if (!Array.isArray(hosts)) return null;
    for (const host of hosts) {
        const block = othersOffOf(host);
        if (block?.allowed === true) return { inherited: block.inherited === true };
    }
    return null;
}

/** Whether the box shows ticked: the browser's choice, else the inherited default. */
export function othersOffChecked(choice, offer) {
    if (typeof choice === 'boolean') return choice;
    return offer?.inherited === true;
}
