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


def has_text(fragment, timeout=30):
    needle = fragment.lower()
    end = time.time() + timeout
    while time.time() < end:
        for w in all_setup_windows():
            for c in w.descendants():
                try:
                    if not c.is_visible():
                        continue
                    txt = (c.window_text() or "").lower().replace("&", "")
                    if needle in txt:
                        return True
                except Exception:
                    pass
        time.sleep(0.2)
    return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--msi", required=True)
    args = p.parse_args()

    proc = subprocess.Popen(["msiexec", "/i", args.msi])
    try:
        click_button("next", 20)
        click_button("next", 20)
        if not click_button("change", 30):
            raise RuntimeError("Could not click maintenance Change")
        if not click_button("next", 30):
            raise RuntimeError("Could not click Next on customize dialog")
        if not has_text("ready to change", 30):
            raise RuntimeError("Did not reach Ready to change dialog")
        print("PASS: change route executed")
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
