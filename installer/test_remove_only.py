import argparse
import subprocess
import sys
import time

from pywinauto import Desktop


def all_setup_windows():
    out = []
    d = Desktop(backend="win32")
    for w in d.windows():
        try:
            if (w.window_text() or "").endswith("Setup"):
                out.append(w)
        except Exception:
            pass
    return out


def click_button(text_part, timeout=40):
    needle = text_part.lower()
    end = time.time() + timeout
    while time.time() < end:
        for w in all_setup_windows():
            for b in w.descendants(class_name="Button"):
                try:
                    if not b.is_visible():
                        continue
                    txt = (b.window_text() or "").lower().replace("&", "")
                    if needle in txt:
                        b.click()
                        return True
                except Exception:
                    pass
        time.sleep(0.2)
    return False


def wait_close(timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        if not all_setup_windows():
            return True
        time.sleep(0.25)
    return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--msi", required=True)
    p.add_argument("--log", default="")
    args = p.parse_args()

    cmd = ["msiexec", "/i", args.msi]
    if args.log:
        cmd.extend(["/l*v", args.log])
    proc = subprocess.Popen(cmd)
    try:
        # reach maintenance area
        click_button("next", 20)
        click_button("next", 20)
        if not click_button("remove", 30):
            raise RuntimeError("Could not click maintenance Remove")
        if not (click_button("remove", 30) or click_button("install", 30)):
            raise RuntimeError("Could not click final remove on ready dialog")
        if not wait_close(180):
            raise RuntimeError("Installer did not close after remove")
        print("PASS: remove executed")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            proc.poll()
            if proc.returncode is None:
                proc.terminate()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
