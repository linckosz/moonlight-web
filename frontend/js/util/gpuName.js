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

/**
 * gpuName — a GPU's name as a host card has room for it.
 *
 * No vendor publishes a short name: the system says "NVIDIA GeForce RTX 5060 Ti"
 * or "AMD Radeon(TM) Graphics", and the card's narrow line cut the number off.
 * So the words that tell two GPUs apart stay, and the rest goes: the vendor,
 * the trademark marks, the brand where the family says it (RTX, GTX, GT, MX,
 * RX), a closing "GPU", a closing "Graphics" after a model number. "Laptop", the
 * memory size, Ti, SUPER and XT stay, for they name another GPU. Display only:
 * the driver is given the full name.
 */

const MARKS = /\s*(?:\((?:R|TM|C)\)|®|™)/gi;
const VENDOR = /^(?:NVIDIA|AMD|ATI|Intel|Apple)\s+/i;
const BRAND = /^(?:GeForce\s+(?=(?:RTX|GTX|GTS|GT|MX)[\s\d])|Radeon\s+(?=RX\s))/i;

/**
 * @param {string} name the name the system gives
 * @returns {string} the short one; the name itself when nothing would be left
 */
export function shortGpuName(name) {
    if (typeof name !== 'string') return '';
    const full = name.trim().replace(/\s+/g, ' ');
    const bare = full.replace(MARKS, '').replace(VENDOR, '').replace(BRAND, '');
    const short = bare
        .replace(/\s+GPU$/i, '')
        .replace(/(\d\w*)\s+Graphics$/i, '$1')
        .trim();
    return short && short.toLowerCase() !== 'gpu' ? short : full;
}

/**
 * The names of one list, each as short as it can be while they stay apart: two
 * different names that would read the same keep their full names, so a list
 * never offers the same line twice for two different GPUs.
 * @param {string[]} names
 * @returns {Map<string, string>} from each full name to the one to show
 */
export function shortGpuNames(names) {
    const seen = new Map();
    for (const name of new Set(names)) {
        const short = shortGpuName(name);
        seen.set(short, (seen.get(short) || 0) + 1);
    }
    const shown = new Map();
    for (const name of names) {
        const short = shortGpuName(name);
        shown.set(name, seen.get(short) > 1 ? name : short);
    }
    return shown;
}
