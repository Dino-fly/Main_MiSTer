#!/usr/bin/env python3
"""
Which identifier off a disc actually finds the game on ScreenScraper.

The question this answers is not "can we scrape" - files already scrape fine by name.
It is: a disc in a tray has no filename, so what DOES it have that ScreenScraper will
accept? Three candidates per disc, measured rather than assumed:

  A  serialnum   the product number printed in the disc header (MK-81207, SLUS-00594)
  B  romnom      the disc's own internal label, exactly as the header spells it
  C  romnom      the same label, cleaned the way a human would read it

Ground truth is the Redump folder name on the card, which is why the sample is drawn
from files rather than from discs in a drive: 52 known-correct answers beats two.

Respecting the account, in order of how much each matters:

  - maxthreads is 1 on this account, so requests are strictly serial. No pool, no
    overlap, one in flight ever.
  - GAP seconds between requests, above what the interface requires, because nothing
    is waiting on this and a study is not a user.
  - BUDGET is a hard ceiling checked before every call, not a target. It cannot run
    away if a loop is wrong.
  - Any answer that mentions the quota, a closed API, or too many requests aborts the
    whole run at once rather than retrying. A retry against a refusal is how an
    account gets banned, and the refusal is the one answer worth obeying immediately.

A miss costs one request against the unmatched allowance, which is the scarce one
(2000/day against 20000). The whole run is budgeted well inside a single day's worth
of the scarce number even if every single lookup misses.
"""
import json, os, sys, time, urllib.parse, urllib.request

BUDGET = 200          # hard ceiling on requests for the entire run
GAP    = 1.5          # seconds between requests
TIMEOUT = 30

API = "https://api.screenscraper.fr/api2/jeuInfos.php"
SYS = {"saturn": "22", "psx": "57"}

spent = 0
aborted = None

REFUSALS = ("quota", "closed", "ferm", "maximum", "trop de", "too many",
            "non autoris", "not authorized", "limite")


def creds():
    env = {}
    with open(os.path.expanduser("~/.config/classicui/ss.env")) as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    return env


E = creds()


def ask(params):
    """One request. Returns (status, payload). Never prints a URL - it carries secrets."""
    global spent, aborted
    if aborted:
        return "aborted", aborted
    if spent >= BUDGET:
        aborted = "budget"
        return "aborted", "budget reached"

    q = dict(devid=E["SS_DEVID"], devpassword=E["SS_DEVPASS"],
             softname=E.get("SS_SOFTNAME", "classichome"),
             output="json", ssid=E["SS_USER"], sspassword=E["SS_PASS"],
             romtype="rom", **params)
    url = API + "?" + urllib.parse.urlencode(q)

    spent += 1
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT) as r:
            body = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace") if e.fp else ""
        low = body.lower()
        if any(w in low for w in REFUSALS):
            aborted = f"refused by the API: {body[:160]}"
            return "aborted", aborted
        if e.code == 404:
            return "miss", ""
        return "http", f"{e.code} {body[:120]}"
    except Exception as e:
        return "error", str(e)[:120]
    finally:
        time.sleep(GAP)

    low = body.lower()
    if any(w in low for w in REFUSALS) and not body.lstrip().startswith("{"):
        aborted = f"refused by the API: {body[:160]}"
        return "aborted", aborted

    try:
        d = json.loads(body)
    except Exception:
        if "aucun" in low or "not found" in low or "rien" in low:
            return "miss", body[:80]
        return "unparsed", body[:120]

    g = d.get("response", {}).get("jeu")
    if not g:
        return "miss", ""
    return "hit", g


def name_of(g):
    n = g.get("noms")
    if isinstance(n, list):
        pref = {}
        for e in n:
            pref[e.get("region", "")] = e.get("text", "")
        for r in ("ss", "us", "wor", "eu", "jp", ""):
            if pref.get(r):
                return pref[r]
        return n[0].get("text", "")
    return str(n)


def clean(label):
    """The label as a person would read it off the box."""
    s = label.replace("_", " ").replace(":", " ")
    s = " ".join(s.split())
    for junk in ("(SEGA SA", "(SEGA", "J:AZEL "):
        s = s.replace(junk, " ")
    s = " ".join(s.split())
    return s.title() if s.isupper() else s


def truth_ok(entry, got):
    """Does the returned game plausibly correspond to the folder we took the disc from."""
    def norm(s):
        s = s.lower()
        for ch in "-_:,.!'&()[]":
            s = s.replace(ch, " ")
        stop = {"the", "a", "of", "usa", "europe", "japan", "disc", "rev", "en", "fr",
                "de", "es", "it", "brazil", "canada", "and"}
        return {w for w in s.split() if w and w not in stop and not w.isdigit()}
    a, b = norm(entry), norm(got)
    if not a or not b:
        return False
    return len(a & b) >= max(1, min(len(a), len(b)) // 2)


rows = [json.loads(l) for l in open(sys.argv[1])]
rows = [r for r in rows if "error" not in r and r.get("hdr") in SYS]

# Order the sample so that the interesting cases are reached first: if the run is cut
# short for any reason, what survives is the part that discriminates.
def interest(r):
    lab = (r.get("label") or r.get("volume") or "")
    return (0 if r.get("serial") else 1, 0 if lab else 1, r["entry"])

rows.sort(key=interest)

if len(sys.argv) > 2:
    rows = rows[:int(sys.argv[2])]

out = []
print(f"{len(rows)} discs, budget {BUDGET} requests, {GAP}s apart\n", flush=True)

for r in rows:
    sysid = SYS[r["hdr"]]
    label = r.get("label") or r.get("volume") or ""
    serial = (r.get("serial") or "").upper()
    rec = {"entry": r["entry"], "hdr": r["hdr"], "serial": serial, "label": label}

    trials = []
    # serialnum ALONE. The first cut of this passed romnom=serial alongside it, which
    # hides the answer both ways: on PSX the serial lookup works and the romnom is
    # redundant, and on Saturn the romnom is what produced the hit while the serial
    # contributed nothing. Worse, romnom=<serial> on its own returned a confidently
    # wrong game (SLUS-00594 -> "Beyblade Burst"), so the two must never be conflated.
    if serial:
        trials.append(("A_serial", {"systemeid": sysid, "serialnum": serial}))
    if label:
        trials.append(("B_label_raw", {"systemeid": sysid, "romnom": label}))
        c = clean(label)
        if c and c != label:
            trials.append(("C_label_clean", {"systemeid": sysid, "romnom": c}))

    for tag, params in trials:
        st, payload = ask(params)
        if st == "aborted":
            print(f"\nABORTED: {payload}")
            rec[tag] = {"status": "aborted"}
            out.append(rec)
            json.dump(out, open("/Users/derek/.claude/jobs/b563830b/tmp/ssmatch.json", "w"), indent=1)
            print(f"spent {spent} requests")
            sys.exit(2)
        if st == "hit":
            got = name_of(payload)
            ok = truth_ok(r["entry"], got)
            rec[tag] = {"status": "hit", "got": got, "correct": ok,
                        "id": payload.get("id")}
            print(f"  {r['entry'][:34]:34} {tag:14} {'OK ' if ok else 'WRONG'} -> {got[:38]}", flush=True)
        else:
            rec[tag] = {"status": st, "detail": str(payload)[:80]}
            print(f"  {r['entry'][:34]:34} {tag:14} {st}", flush=True)

    out.append(rec)
    json.dump(out, open("/Users/derek/.claude/jobs/b563830b/tmp/ssmatch.json", "w"), indent=1)

print(f"\nspent {spent} requests of {BUDGET}")
