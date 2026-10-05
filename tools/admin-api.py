#!/usr/bin/env python3
"""One call to a receiver's admin API, signed in as docs/DEPLOYMENT.md
("Admin API authentication") describes, for the labs.

    tools/admin-api.py BASE PASSWORD METHOD PATH [BODY]
    tools/admin-api.py http://127.0.0.1:8073 'secret' GET /api/admin/update

Signs in afresh each time: an update restarts the receiver, and its
sessions end with it. Prints the answer's body, or {} when the receiver does
not answer, and exits non-zero unless the answer was a success.
"""
import hashlib
import hmac
import json
import sys
import urllib.error
import urllib.request


def call(base, method, path, body=b"", headers=None):
    request = urllib.request.Request(base + path, data=body if method != "GET" else None, method=method,
                                     headers={"Content-Type": "application/json", **(headers or {})})
    try:
        with urllib.request.urlopen(request, timeout=20) as answer:
            return answer.status, answer.read().decode(), answer.headers
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode(), error.headers
    except OSError:
        return 0, "{}", {}


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    base, password, method, path = sys.argv[1:5]
    body = sys.argv[5].encode() if len(sys.argv) == 6 else b""
    status, text, _ = call(base, "POST", "/api/admin/challenge", b"{}")
    if status != 200:
        print("{}")
        sys.exit(1)
    challenge = json.loads(text)
    # The salt's hex text is the salt, as the panel uses it.
    derived = hashlib.pbkdf2_hmac("sha256", password.encode(), challenge["salt"].encode(),
                                  int(challenge["iterations"]), 32)
    proof = hmac.new(derived, challenge["nonce"].encode(), hashlib.sha256).hexdigest()
    status, text, headers = call(base, "POST", "/api/admin/login",
                                 json.dumps({"nonce": challenge["nonce"], "proof": proof}).encode())
    if status != 200:
        print(json.dumps({"login": status, "answer": text}))
        sys.exit(1)
    cookie = headers["Set-Cookie"].split(";")[0]
    context = json.loads(text)["signing_context"]
    key = hmac.new(derived, ("fernsdr-admin-session-v3\n" + context).encode(), hashlib.sha256).digest()
    sent = {"Cookie": cookie}
    if method != "GET":
        message = ("fernsdr-admin-v3\n%s\n1\n%s\n%s\n" % (context, method, path)).encode() + body
        sent["X-FernSDR-Counter"] = "1"
        sent["X-FernSDR-Signature"] = hmac.new(key, message, hashlib.sha256).hexdigest()
    status, text, _ = call(base, method, path, body, sent)
    print(text)
    sys.exit(0 if 200 <= status < 300 else 1)


if __name__ == "__main__":
    main()
