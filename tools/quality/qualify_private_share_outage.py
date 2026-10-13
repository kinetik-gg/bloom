#!/usr/bin/env python3
"""Opt-in Linux SSHFS outage proof using only disposable loopback infrastructure.

Build the official libfuse/sshfs release privately, and pass its executable with --sshfs.
Build bloom_runtime_private_share_outage_probe in a CPU/stub tree. --fixtures names the
generated media-worker fixture directory from that same tree. No user SSH settings,
existing shares, desktop displays, or GPU execution are used.
"""

import argparse
import getpass
import json
import os
from pathlib import Path
import selectors
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def mounted(path):
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        before, after = line.split(" - ", 1)
        name = before.split()[4].replace(r"\040", " ").replace(r"\134", "\\")
        if name == str(path):
            require(after.split()[0] == "fuse.sshfs", "private path has an unexpected mount type")
            return True
    return False


def terminate_owned(process, witness):
    if process is None or process.poll() is not None:
        return
    try:
        command = Path(f"/proc/{process.pid}/cmdline").read_bytes().replace(b"\0", b" ")
    except FileNotFoundError:
        process.wait(timeout=1)
        return
    if not command.strip():
        process.wait(timeout=1)  # The owned child exited between poll() and the /proc read.
        return
    require(os.fsencode(str(witness)) in command, "recorded process no longer matches private owner")
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


class Transcript:
    def __init__(self, process, log):
        self.process = process
        self.log = log
        self.buffer = b""
        self.selector = selectors.DefaultSelector()
        self.selector.register(process.stdout, selectors.EVENT_READ)

    def until(self, expected, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace")
                self.log.write(text + "\n")
                self.log.flush()
                print(text, flush=True)
                if text == expected or (expected == "PASS" and text.startswith("PASS:")):
                    return
                require(not text.startswith("FAIL:"), "production outage probe failed")
            ready = self.selector.select(min(1, max(0, deadline - time.monotonic())))
            if ready:
                chunk = os.read(self.process.stdout.fileno(), 65536)
                require(chunk, "production outage probe ended before its expected checkpoint")
                self.buffer += chunk
        raise RuntimeError("production outage probe exceeded its checkpoint deadline")


def run(arguments):
    require(os.name == "posix" and Path("/proc/self/mountinfo").exists(), "Linux is required")
    os.umask(0o077)
    probe = Path(arguments.probe).resolve(strict=True)
    sshfs = Path(arguments.sshfs).resolve(strict=True)
    fixtures = Path(arguments.fixtures).resolve(strict=True)
    evidence = Path(arguments.evidence_root).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="private-share-", dir=evidence))
    root.chmod(0o700)
    served, mount, cache = (root / name for name in ("served", "mount", "cache"))
    for path in (served, mount, cache):
        path.mkdir(mode=0o700)
    # OpenSSH StrictModes checks every ancestor of authorized_keys. Keep disposable keys under
    # the user's home even when the evidence workspace has a group-writable ancestor; ~/.ssh is
    # never read or modified, and this fresh directory is removed during teardown.
    keys = Path(tempfile.mkdtemp(prefix=".bloom-private-share-", dir=Path.home()))
    keys.chmod(0o700)
    host, client = keys / "host-key", keys / "client-key"
    authorized, known = keys / "authorized-keys", keys / "known-hosts"
    config, wrapper, transport_file = root / "sshd-config", root / "ssh-transport", root / "transport-pid"
    environment = dict(os.environ, XDG_CACHE_HOME=str(cache))
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    daemon = filesystem = exercise = None
    owned_filesystems = []
    logs = []
    receipt = {"schema": 1, "transport": "SSHFS", "scope": "privateLinuxLoopback",
               "smbClaim": False, "gpuExecution": False, "passed": False}
    try:
        for key in (host, client):
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)], check=True)
        authorized.write_text('restrict,command="internal-sftp" ' + client.with_suffix(".pub").read_text())
        with socket.socket() as available:
            available.bind(("127.0.0.1", 0))
            port = available.getsockname()[1]
        known.write_text(f"[127.0.0.1]:{port} " + host.with_suffix(".pub").read_text())
        config.write_text("\n".join([
            f"Port {port}", "ListenAddress 127.0.0.1", f"HostKey {host}",
            f"PidFile {root / 'sshd-pid'}", f"AuthorizedKeysFile {authorized}",
            "PasswordAuthentication no", "KbdInteractiveAuthentication no", "UsePAM no",
            "PubkeyAuthentication yes", "StrictModes yes", "PermitUserEnvironment no",
            "AllowTcpForwarding no", "X11Forwarding no", "PermitTunnel no",
            f"AllowUsers {getpass.getuser()}", "Subsystem sftp internal-sftp", "LogLevel VERBOSE", ""]))
        wrapper.write_text("#!/bin/sh\n" + f"printf '%s\\n' \"$$\" > {shlex.quote(str(transport_file))}\n"
                           + "exec /usr/bin/ssh -F /dev/null \"$@\"\n")
        wrapper.chmod(0o700)
        subprocess.run(["/usr/bin/sshd", "-t", "-f", str(config)], check=True)
        daemon_log = (root / "sshd.log").open("wb")
        logs.append(daemon_log)
        daemon = subprocess.Popen(["/usr/bin/sshd", "-D", "-e", "-f", str(config)],
                                  stdout=daemon_log, stderr=subprocess.STDOUT)
        subprocess.run([str(probe), "--prepare", str(served), str(fixtures)], env=environment, check=True)
        stamps = {name: (served / name).stat() for name in ("still.exr", "video.mov")}

        def connect():
            require(not mounted(mount), "only a new empty private mount may be mounted")
            transport_file.unlink(missing_ok=True)
            output = (root / f"sshfs-{time.monotonic_ns()}.log").open("wb")
            logs.append(output)
            command = [str(sshfs), "-f", "-p", str(port), "-o",
                       "ro,dir_cache=no,direct_io,attr_timeout=0,entry_timeout=0,negative_timeout=0",
                       "-o", f"ssh_command={wrapper}", "-o", f"IdentityFile={client}",
                       "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                       "-o", "StrictHostKeyChecking=yes", "-o", f"UserKnownHostsFile={known}",
                       "-o", "GlobalKnownHostsFile=/dev/null", "-o", "ConnectTimeout=3",
                       "-o", "ServerAliveInterval=1", "-o", "ServerAliveCountMax=1",
                       f"{getpass.getuser()}@127.0.0.1:{served}", str(mount)]
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
            owned_filesystems.append(process)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                require(process.poll() is None, "private SSHFS failed; inspect its private log")
                if mounted(mount) and transport_file.exists():
                    try:
                        with (mount / "still.exr").open("rb") as source:
                            require(source.read(4) == b"\x76\x2f\x31\x01", "mounted fixture has real EXR magic")
                        receipt["mountVerified"] = True
                        return process
                    except OSError:
                        pass
                time.sleep(0.05)
            raise RuntimeError("private SSHFS mount did not become available")

        def disconnect():
            pid = int(transport_file.read_text().strip())
            process_root = Path(f"/proc/{pid}")
            require(process_root.stat().st_uid == os.getuid(), "private transport belongs to another user")
            command = (process_root / "cmdline").read_bytes().split(b"\0")
            require((process_root / "exe").resolve() == Path("/usr/bin/ssh").resolve() and
                    os.fsencode(str(client)) in b" ".join(command) and
                    os.fsencode(str(port)) in b" ".join(command) and b"127.0.0.1" in b" ".join(command),
                    "transport PID lacks the exact private port/identity/loopback witnesses")
            owned_command = Path(f"/proc/{filesystem.pid}/cmdline").read_bytes()
            require(os.fsencode(str(sshfs)) in owned_command and os.fsencode(str(mount)) in owned_command,
                    "private FUSE daemon PID lacks the exact executable/mount witnesses")
            # Upstream SSHFS normally cleanly unmounts itself after transport loss. Stop its
            # handler first, terminate the actual SSH transport, and abruptly exit only this
            # private daemon so the kernel retains an ENOTCONN mount for the outage interval.
            filesystem.send_signal(signal.SIGSTOP)
            os.kill(pid, signal.SIGTERM)
            filesystem.kill()
            filesystem.wait(timeout=5)
            receipt["outageKind"] = "sshTransportLossAndAbruptPrivateFuseClientExit"
            require(mounted(mount), "outage must leave an actual disconnected SSHFS mount")
            observed = subprocess.run([sys.executable, "-c",
                "import json,sys\ntry:\n open(sys.argv[1],'rb').read(1)\n"
                "except OSError as e:\n print(json.dumps({'errno':e.errno}))\n"
                "else:\n raise SystemExit('disconnected share still served bytes')\n",
                str(mount / "still.exr")], capture_output=True, text=True, timeout=5, check=True)
            receipt["outageErrno"] = json.loads(observed.stdout)["errno"]

        filesystem = connect()
        probe_log = (root / "probe.log").open("w")
        logs.append(probe_log)
        exercise = subprocess.Popen([str(probe), "--exercise", str(mount), str(cache / "spool")],
                                    env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
        transcript = Transcript(exercise, probe_log)
        transcript.until("READY_OUTAGE")
        disconnect()
        exercise.stdin.write(b"OUTAGE\n")
        exercise.stdin.flush()
        transcript.until("READY_RECONNECT")
        for current, replacement in (("still.exr", "replacement.exr"), ("video.mov", "replacement.mov")):
            shutil.copyfile(served / replacement, served / current)
            old = stamps[current]
            os.utime(served / current, ns=(old.st_atime_ns, old.st_mtime_ns))
        require(mounted(mount), "only our verified disconnected mount may be unmounted")
        subprocess.run(["/usr/bin/fusermount3", "-u", str(mount)], check=True)
        filesystem = connect()
        exercise.stdin.write(b"RECONNECTED\n")
        exercise.stdin.flush()
        transcript.until("PASS")
        require(exercise.wait(timeout=15) == 0, "production outage proof exits successfully")
        receipt["passed"] = True
    finally:
        cleanup_errors = []

        def cleanup(action):
            try:
                action()
            except Exception as error:
                cleanup_errors.append(type(error).__name__)

        if exercise is not None and exercise.poll() is None:
            exercise.terminate()
            try:
                exercise.wait(timeout=5)
            except subprocess.TimeoutExpired:
                exercise.kill()
                exercise.wait(timeout=5)
        if mounted(mount):
            cleanup(lambda: subprocess.run(["/usr/bin/fusermount3", "-u", str(mount)], check=True))
        for owned in owned_filesystems:
            cleanup(lambda owned=owned: terminate_owned(owned, sshfs))
        cleanup(lambda: terminate_owned(daemon, config))
        for log in logs:
            log.close()
        for key in (host, host.with_suffix(".pub"), client, client.with_suffix(".pub"), authorized, known):
            key.unlink(missing_ok=True)
        keys.rmdir()
        receipt["privateMountRemoved"] = not mounted(mount)
        receipt["temporaryKeysRemoved"] = all(not key.exists() for key in (host, client, authorized))
        receipt["ownedProcessesStopped"] = (daemon is None or daemon.poll() is not None) and all(
            owned.poll() is not None for owned in owned_filesystems)
        receipt["cleanupErrors"] = cleanup_errors
        receipt["probePassed"] = receipt["passed"]
        receipt["passed"] = receipt["passed"] and receipt["privateMountRemoved"] and \
            receipt["temporaryKeysRemoved"] and receipt["ownedProcessesStopped"] and not cleanup_errors
        (root / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    require(receipt["privateMountRemoved"] and receipt["temporaryKeysRemoved"] and
            receipt["ownedProcessesStopped"] and not receipt["cleanupErrors"],
            "private infrastructure cleanup did not complete")
    print(json.dumps(receipt, sort_keys=True))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sshfs", required=True)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--evidence-root", required=True)
    run(parser.parse_args())
