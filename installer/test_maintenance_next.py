import argparse
import subprocess
import sys
import time

from pywinauto import Desktop


def wait_setup_window(timeout=30):
    end = time.time() + timeout
    desktop = Desktop(backend="win32")
    while time.time() < end:
        for w in desktop.windows():
            try:
                if w.window_text() and w.window_text().endswith("Setup"):
                    return w
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("Timed out waiting for setup window")


def click_next(window, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for b in window.descendants():
            try:
                text = (b.window_text() or "").lower()
                if b.is_visible() and "next" in text:
                    b.click()
                    return
            except Exception:
                pass
        time.sleep(0.2)
    labels = []
    for b in window.descendants():
        try:
            text = (b.window_text() or "").strip()
            if text:
                labels.append(text)
        except Exception:
            pass
    print("Visible texts:", labels)
    raise RuntimeError("Next button not found")


def has_maintenance_controls(window):
    labels = []
    texts = []
    for b in window.descendants(class_name="Button"):
        try:
            if b.is_visible():
                labels.append((b.window_text() or "").lower())
        except Exception:
            pass
    for c in window.descendants():
        try:
            if c.is_visible():
                t = (c.window_text() or "").strip()
                if t:
                    texts.append(t.lower())
        except Exception:
            pass
    return (
        (any("change" in t for t in labels) and any("repair" in t for t in labels) and any("remove" in t for t in labels))
        or any("modify, repair, or remove" in t for t in texts)
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msi", required=True)
    args = parser.parse_args()

    p = subprocess.Popen(["msiexec", "/i", args.msi])
    try:
        win = wait_setup_window(30)
        click_next(win, 20)
        end = time.time() + 20
        while time.time() < end:
            win = wait_setup_window(2)
            if has_maintenance_controls(win):
                print("PASS: Welcome Next reached maintenance dialog")
                return 0
            time.sleep(0.2)
        dump = []
        for c in win.descendants():
            try:
                if c.is_visible():
                    t = (c.window_text() or "").strip()
                    if t:
                        dump.append(t)
            except Exception:
                pass
        print("Visible texts after Next:", dump)
        print("FAIL: maintenance dialog controls not found after Next")
        return 1
    finally:
        try:
            p.terminate()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
    labels = [t.replace("&", "") for t in labels]
