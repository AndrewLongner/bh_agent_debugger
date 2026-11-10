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


def click_button_contains(window, text_part, timeout=20):
    end = time.time() + timeout
    needle = text_part.lower()
    while time.time() < end:
        for b in window.descendants(class_name="Button"):
            try:
                text = (b.window_text() or "").lower().replace("&", "")
                if b.is_visible() and needle in text:
                    b.click()
                    return
            except Exception:
                pass
        time.sleep(0.2)
    texts = []
    for c in window.descendants():
        try:
            if c.is_visible():
                t = (c.window_text() or "").strip()
                if t:
                    texts.append(t)
        except Exception:
            pass
    print("Visible texts:", texts)
    raise RuntimeError(f"Button containing '{text_part}' not found")


def visible_texts(window):
    out = []
    for c in window.descendants():
        try:
            if c.is_visible():
                t = (c.window_text() or "").strip()
                if t:
                    out.append(t.lower().replace("&", ""))
        except Exception:
            pass
    return out


def verify_action(msi, action, execute_remove=False):
    p = subprocess.Popen(["msiexec", "/i", msi])
    try:
        w = wait_setup_window(30)
        # Depending on product state we may land on Welcome, MaintenanceWelcome, or MaintenanceType.
        for _ in range(3):
            texts = visible_texts(w)
            if any("change" in t for t in texts) and any("repair" in t for t in texts) and any("remove" in t for t in texts):
                break
            click_button_contains(w, "Next", 20)
            w = wait_setup_window(10)

        click_button_contains(w, action, 20)   # action button -> verify ready

        if action == "change":
            # Change now routes through feature-selection dialog first.
            w = wait_setup_window(10)
            click_button_contains(w, "next", 20)

        end = time.time() + 20
        while time.time() < end:
            w = wait_setup_window(2)
            texts = visible_texts(w)
            if any(f"ready to {action}" in t for t in texts):
                if action == "remove" and execute_remove:
                    if not click_and_return(w, "remove", 10):
                        click_and_return(w, "install", 10)
                    if not wait_window_close(120):
                        print("FAIL: remove did not finish")
                        return False
                print(f"PASS: {action}")
                return True
            time.sleep(0.2)
        print(f"FAIL: {action} did not reach ready dialog")
        return False
    finally:
        try:
            p.terminate()
        except Exception:
            pass


def click_and_return(window, text_part, timeout=10):
    try:
        click_button_contains(window, text_part, timeout)
        return True
    except Exception:
        return False


def wait_window_close(timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        desktop = Desktop(backend="win32")
        if not any((w.window_text() or "").endswith("Setup") for w in desktop.windows()):
            return True
        time.sleep(0.25)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msi", required=True)
    parser.add_argument("--execute-remove", action="store_true")
    args = parser.parse_args()

    ok = True
    for action in ("change", "repair", "remove"):
        ok = verify_action(args.msi, action, args.execute_remove) and ok

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
