#!/usr/bin/env python3
"""Drives achivmentsd with rcheevos-shaped requests against a fake RetroAchievements server.

Usage: integration.py PATH_TO_ACHIVMENTSD
"""
import hashlib
import http.server
import json
import os
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

EMULATOR_UA = "RetroArch/1.21.0 (Linux 6.6) mgba_libretro/0.10.3"
ACHD_UA = "achivmentsd/0.1"

ACHIEVEMENT_SETS = {
    "Success": True, "GameId": 12, "Title": "Test Game", "ConsoleId": 5, "ImageIconUrl": "x",
    "Sets": [{"AchievementSetId": 1, "GameId": 12, "Title": "Test Game", "Type": "core",
              "ImageIconUrl": "x", "Achievements": [{"ID": 5}, {"ID": 6}, {"ID": 7}, {"ID": 999}],
              "Leaderboards": []}],
}


class FakeRA:
    def __init__(self, port=0):
        self.requests = []
        fake = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                if "chunked" in (self.headers.get("Transfer-Encoding") or ""):
                    return self.reply(422, {"Success": False, "Error": "chunked body"})
                length = int(self.headers.get("Content-Length") or 0)
                form = dict(urllib.parse.parse_qsl(self.rfile.read(length).decode()))
                fake.requests.append({"form": form, "ua": self.headers.get("User-Agent")})
                status, body = fake.respond(form)
                self.reply(status, body)

            def reply(self, status, body):
                data = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        http.server.ThreadingHTTPServer.allow_reuse_address = True
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def respond(self, form):
        r = form.get("r")
        if r == "login2":
            if form.get("p") == "bad":
                return 401, {"Success": False, "Status": 401, "Code": "invalid_credentials",
                             "Error": "Invalid user/password combination. Please try again."}
            return 200, {"Success": True, "User": "Bob", "Token": "tok123", "Score": 100, "SoftcoreScore": 5}
        if r == "achievementsets":
            return 200, ACHIEVEMENT_SETS
        if r == "startsession":
            return 200, {"Success": True, "Unlocks": [{"ID": 5, "When": 1700000000}],
                         "HardcoreUnlocks": [], "ServerNow": 1700000100}
        if r == "unlocks":
            return 200, {"Success": True, "UserUnlocks": [5, 6]}
        if r == "awardachievement":
            if form.get("a") == "999":
                return 404, {"Success": False, "Status": 404, "Code": "not_found", "Error": "Not found."}
            return 200, {"Success": True, "Score": 110, "SoftcoreScore": 5,
                         "AchievementID": int(form["a"]), "AchievementsRemaining": 2}
        if r == "submitlbentry":
            return 200, {"Success": True, "Response": {"Score": int(form["s"]), "BestScore": int(form["s"]),
                                                       "RankInfo": {"Rank": 1, "NumEntries": "1"},
                                                       "TopEntries": []}}
        if r in ("ping", "postactivity"):
            return 200, {"Success": True}
        return 400, {"Success": False, "Error": "unknown action"}

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    def find(self, action, **match):
        return [q for q in self.requests if q["form"].get("r") == action
                and all(q["form"].get(k) == v for k, v in match.items())]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def call(port, **form):
    data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/dorequest.php", data=data,
                                 headers={"User-Agent": EMULATOR_UA,
                                          "Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def wait_for(pred, timeout=10.0, what="condition"):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def awards(state, status):
    db = sqlite3.connect(f"file:{os.path.join(state, 'achivmentsd.db')}?mode=ro", uri=True)
    try:
        return db.execute("SELECT achievement_id, last_error FROM awards WHERE status = ? ORDER BY seq",
                          (status,)).fetchall()
    finally:
        db.close()


def queued(state):
    return awards(state, 0)


def main():
    daemon = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="achd-it-")
    state = os.path.join(work, "state")
    log = open(os.path.join(work, "daemon.log"), "w+")
    ra = FakeRA()
    ra_port = ra.port
    port = free_port()
    proc = subprocess.Popen([daemon, "-c", _write_config(work),
                             "-s", state, "-p", str(port), "-u", f"http://127.0.0.1:{ra_port}", "-v"],
                            stdout=log, stderr=log)
    ok = False
    try:
        wait_for(lambda: _listening(port), what="daemon to listen")

        # --- online: relayed and cached -------------------------------------------------
        status, body = call(port, r="login2", u="Bob", p="pw")
        assert status == 200 and body["Token"] == "tok123", body
        status, body = call(port, r="login2", u="Bob", p="bad")
        assert status == 401 and body["Code"] == "invalid_credentials", (status, body)
        status, body = call(port, r="achievementsets", u="Bob", t="tok123", m="ABC123")
        assert status == 200 and body["GameId"] == 12, body
        status, body = call(port, r="startsession", u="Bob", t="tok123", g="12", h="0", m="ABC123")
        assert status == 200 and body["Unlocks"][0]["ID"] == 5, body
        status, body = call(port, r="awardachievement", u="Bob", t="tok123", a="6", h="0", v=md5("6Bob0"))
        assert status == 200 and body["Success"], body
        live = ra.find("awardachievement", a="6")
        assert live and live[0]["ua"] == EMULATOR_UA, live  # pass-through keeps the emulator's UA

        # --- offline: served from cache, awards queued ------------------------------------
        ra.stop()
        status, body = call(port, r="ping", u="Bob", t="tok123", g="12")
        assert status == 200 and body == {"Success": True}, (status, body)
        status, body = call(port, r="login2", u="bob", t="tok123")
        assert status == 200 and body["Token"] == "tok123", body
        status, body = call(port, r="achievementsets", u="Bob", t="tok123", m="abc123")
        assert status == 200 and body["Sets"][0]["Achievements"][0]["ID"] == 5, body
        status, body = call(port, r="gameid", m="ffff")
        assert status == 200 and body["GameID"] == 0 and not body["Success"], body
        status, body = call(port, r="awardachievement", u="Bob", t="tok123", a="7", h="1", v=md5("7Bob1"))
        assert status == 200 and body["Success"] and body["AchievementID"] == 7, body
        status, body = call(port, r="awardachievement", u="Bob", t="tok123", a="7", h="1", v=md5("7Bob1"))
        assert status == 200 and body["Success"], body  # duplicate: answered, not queued twice
        status, body = call(port, r="awardachievement", u="Bob", t="tok123", a="999", h="0", v=md5("999Bob0"))
        assert status == 200 and body["Success"], body
        status, body = call(port, r="submitlbentry", u="Bob", t="tok123", i="77", s="-5", v=md5("77Bob-5"))
        assert status == 200 and body["Response"]["Score"] == -5 and body["Response"]["RankInfo"]["NumEntries"] == "1", body
        assert len(queued(state)) == 3, queued(state)

        status, body = call(port, r="startsession", u="Bob", t="tok123", g="12", h="1")
        soft = {u["ID"] for u in body["Unlocks"]}
        hard = {u["ID"] for u in body["HardcoreUnlocks"]}
        assert status == 200 and {5, 7, 999} <= soft and hard == {7}, body
        status, body = call(port, r="unlocks", u="Bob", t="tok123", g="12", h="1")
        assert status == 200 and body["UserUnlocks"] == [7], body
        status, body = call(port, r="patch", u="Bob", t="tok123", g="404")
        assert status == 503, (status, body)

        # --- reconnect: queue replayed with backdate and our UA token ---------------------
        time.sleep(2)
        ra = FakeRA(ra_port)
        proc.send_signal(signal.SIGUSR1)
        wait_for(lambda: not queued(state), timeout=15, what="queue to drain")

        replay = ra.find("awardachievement", a="7")
        assert len(replay) == 1, ra.requests
        form, ua = replay[0]["form"], replay[0]["ua"]
        o = form["o"]
        assert int(o) >= 2 and form["h"] == "1" and form["t"] == "tok123", form
        assert form["v"] == md5("7Bob17" + o), form
        assert ua == f"{EMULATOR_UA} {ACHD_UA}", ua

        lb = ra.find("submitlbentry", i="77")
        assert len(lb) == 1 and lb[0]["form"]["v"] == md5("77Bob-5" + lb[0]["form"]["o"]), lb

        failed = awards(state, 1)
        assert failed == [(999, "Not found.")], failed

        # Post-replay refresh pulls unlocks with the daemon's own UA.
        wait_for(lambda: ra.find("unlocks", g="12", h="1"), timeout=10, what="unlocks refresh")
        assert ra.find("unlocks", g="12")[0]["ua"] == ACHD_UA

        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=10) == 0
        ok = True
        print("integration: all checks passed")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        ra.stop()
        if not ok:
            log.seek(0)
            sys.stderr.write("---- daemon log ----\n" + log.read())
        log.close()
        shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


def _write_config(work):
    path = os.path.join(work, "config.json")
    with open(path, "w") as f:
        json.dump({"proxy_host": "127.0.0.1"}, f)
    return path


def _listening(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


if __name__ == "__main__":
    sys.exit(main())
