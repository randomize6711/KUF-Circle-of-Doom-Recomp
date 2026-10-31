"""Classifies runtime [error]/[critical] log lines into unexpected and known (config/v1/known_log_errors.toml)."""
import os
import re
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_known(path=os.path.join(ROOT, "config", "v1", "known_log_errors.toml")):
    with open(path, "rb") as f:
        return [(k["id"], re.compile(k["pattern"])) for k in tomllib.load(f).get("known", [])]


def classify(lines, known=None):
    """Returns (unexpected_lines, {issue_id: count}) for the error lines among `lines`."""
    known = load_known() if known is None else known
    unexpected, counts = [], {}
    for line in lines:
        if not re.search(r"\[(error|critical)\]", line):
            continue
        hit = next((kid for kid, rx in known if rx.search(line.rstrip("\n"))), None)
        if hit:
            counts[hit] = counts.get(hit, 0) + 1
        else:
            unexpected.append(line.strip()[:200])
    return unexpected, counts
