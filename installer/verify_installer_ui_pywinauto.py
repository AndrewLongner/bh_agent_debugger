import argparse
import subprocess
import sys
import time
from pathlib import Path

from pywinauto import Desktop


def wait_window(timeout=30):
    end = time.time() + timeout
    desktop = Desktop(backend="win32")
    while time.time() < end:
        for w in desktop.windows():
            try:
                if w.window_text() and w.window_text().endswith("Setup"):
                    return w
            except Exception:
                pass
        time.sleep(0.25)
    raise RuntimeError("Timed out waiting for installer window")


def all_setup_windows():
    out = []
    desktop = Desktop(backend="win32")
    try:
        windows = desktop.windows()
    except Exception:
        return out
    for w in windows:
        try:
            if w.window_text() and w.window_text().endswith("Setup"):
                out.append(w)
        except Exception:
            pass
    return out


def all_windows():
    desktop = Desktop(backend="win32")
    try:
        return desktop.windows()
    except Exception:
        return []


def find_visible(window, auto_id):
    matches = window.descendants()
    for c in matches:
        try:
            aid = ""
            try:
                aid = c.element_info.automation_id
            except Exception:
                aid = ""
            if aid == auto_id and c.is_visible():
                return c
        except Exception:
            continue
    return None


def find_visible_button_by_name(window, name_fragment):
    for c in window.descendants(class_name="Button"):
        try:
            if not c.is_visible():
                continue
            name = (c.window_text() or "").replace("&", "")
            if name_fragment.lower() in name.lower():
                return c
        except Exception:
            continue
    return None


def is_enabled(control):
    try:
        return control.is_enabled()
    except Exception:
        return False


def activate(control):
    try:
        control.click()
        return
    except Exception:
        pass
    try:
        control.invoke()
        return
    except Exception:
        pass
    try:
        control.toggle()
        return
    except Exception:
        pass
    try:
        control.click_input()
        return
    except Exception:
        pass
    raise RuntimeError("Unable to activate control without input simulation")


def click(auto_id, name_fragment=None, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if detect_error_2343():
            raise RuntimeError("Detected MSI UI error: Error 2343")
        for window in all_setup_windows():
            control = find_visible(window, auto_id)
            if control is None and name_fragment:
                control = find_visible_button_by_name(window, name_fragment)
            if control is not None:
                activate(control)
                return
        time.sleep(0.2)
    if name_fragment:
        raise RuntimeError(f"Timed out waiting for clickable control: {auto_id}/{name_fragment}")
    raise RuntimeError(f"Timed out waiting for clickable control: {auto_id}")


def set_checked(auto_id, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if detect_error_2343():
            raise RuntimeError("Detected MSI UI error: Error 2343")
        for window in all_setup_windows():
            control = find_visible(window, auto_id)
            if control is None:
                for c in window.descendants(class_name="Button"):
                    try:
                        if c.is_visible() and "accept" in (c.window_text() or "").lower():
                            control = c
                            break
                    except Exception:
                        continue
            if control is not None:
                try:
                    state = control.get_toggle_state()
                except Exception:
                    state = None
                if state != 1:
                    activate(control)
                return
        time.sleep(0.2)
    dump_visible_controls()
    raise RuntimeError(f"Timed out waiting for checkbox: {auto_id}")


def contains_text(window, text_fragment):
    for c in window.descendants():
        try:
            name = c.window_text() or ""
            if text_fragment.lower() in name.lower():
                return True
        except Exception:
            continue
    return False


def has_maintenance_actions(window):
    labels = []
    for c in window.descendants(class_name="Button"):
        try:
            if not c.is_visible():
                continue
            labels.append((c.window_text() or "").lower().replace("&", ""))
        except Exception:
            continue
    return any("change" in t for t in labels) and any("repair" in t for t in labels) and any("remove" in t for t in labels)


def detect_flow_mode(timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for window in all_setup_windows():
            try:
                if has_maintenance_actions(window):
                    return "maintenance"
                if find_visible(window, "LicenseAcceptedCheckBox") is not None:
                    return "install"
            except Exception:
                continue
        time.sleep(0.2)
    return "install"


def detect_error_2343():
    for window in all_setup_windows():
        if contains_text(window, "error code is 2343") or contains_text(window, "Error 2343"):
            return True
    return False


def find_vs_prompt_window():
    for window in all_windows():
        try:
            title = window.window_text() or ""
            if "BH Agent Debugger Setup" not in title:
                continue
            if contains_text(window, "Visual Studio is still running"):
                return window
        except Exception:
            continue
    return None


def handle_vs_prompt(action, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        window = find_vs_prompt_window()
        if window is not None:
            label = "Cancel" if action == "cancel" else "Retry"
            button = find_visible_button_by_name(window, label)
            if button is None:
                raise RuntimeError(f"VS-running prompt found but '{label}' button not visible")
            try:
                activate(button)
            except Exception:
                try:
                    window.set_focus()
                    if action == "cancel":
                        window.type_keys("%c")
                    else:
                        window.type_keys("%r")
                except Exception:
                    raise
            return True
        time.sleep(0.2)
    return False


def dump_visible_controls():
    print("[pywinauto] Visible controls snapshot:")
    for window in all_setup_windows():
        try:
            print(f"  Window: {window.window_text()}")
            for c in window.descendants():
                try:
                    if not c.is_visible():
                        continue
                    cname = c.friendly_class_name()
                    text = c.window_text() or ""
                    try:
                        aid = c.element_info.automation_id or ""
                    except Exception:
                        aid = ""
                    if text or aid:
                        print(f"    {cname} id='{aid}' text='{text}'")
                except Exception:
                    continue
        except Exception:
            continue


def run(msi_path, timeout_seconds=180, skip_uninstall=False, msi_log_path=None, expect_vs_prompt=False, vs_prompt_action="cancel", maintenance_action="repair"):
    msi = Path(msi_path)
    if not msi.exists():
        raise RuntimeError(f"MSI not found: {msi}")

    print("[pywinauto] Launching interactive installer")
    msiexec_args = [
        "msiexec",
        "/i",
        str(msi),
        "ALLUSERS=2",
        "MSIINSTALLPERUSER=1",
        "WixAppFolder=WixPerUserFolder",
    ]
    if msi_log_path:
        msiexec_args.extend(["/l*v", str(msi_log_path)])
    installer = subprocess.Popen(msiexec_args)

    try:
        saw_vs_prompt = False

        def consume_vs_prompt(timeout=1):
            nonlocal saw_vs_prompt
            if not expect_vs_prompt:
                return
            if handle_vs_prompt(vs_prompt_action, timeout=timeout):
                saw_vs_prompt = True

        wait_window(timeout=30)
        consume_vs_prompt(timeout=1)

        print("[pywinauto] Welcome -> Next")
        click("Next", "Next")
        consume_vs_prompt(timeout=1)

        mode = detect_flow_mode(timeout=20)
        maintenance_mode = mode == "maintenance"
        print(f"[pywinauto] Detected mode: {mode}")

        if maintenance_mode:
            print(f"[pywinauto] Maintenance -> {maintenance_action.title()}")
            click(maintenance_action.title(), maintenance_action)
            consume_vs_prompt(timeout=1)
        else:
            print("[pywinauto] Accept license -> Scope")
            set_checked("LicenseAcceptedCheckBox")
            click("Next", "Next")
            consume_vs_prompt(timeout=1)

            print("[pywinauto] Scope -> Features")
            click("Next", "Next")
            consume_vs_prompt(timeout=1)

            browse_btn = None
            for window in all_setup_windows():
                browse_btn = find_visible(window, "Browse") or find_visible_button_by_name(window, "Browse")
                if browse_btn is not None:
                    break
            if browse_btn is not None:
                print(f"[pywinauto] Features Browse enabled={is_enabled(browse_btn)}")

            print("[pywinauto] Features -> InstallDir")
            click("Next", "Next")
            consume_vs_prompt(timeout=1)

            print("[pywinauto] InstallDir -> VerifyReady")
            click("ChangeFolder", "Change")
            click("OK", "OK")
            click("Next", "Next")
            consume_vs_prompt(timeout=1)

        print("[pywinauto] VerifyReady -> Execute")
        install_btn = None
        for window in all_setup_windows():
            install_btn = find_visible(window, "Install") or find_visible(window, "InstallNoShield")
            if install_btn is None:
                install_btn = find_visible(window, "Repair")
            if install_btn is None:
                install_btn = find_visible(window, "Remove")
            if install_btn is None:
                install_btn = find_visible_button_by_name(window, "Install")
            if install_btn is None:
                install_btn = find_visible_button_by_name(window, "Repair")
            if install_btn is None:
                install_btn = find_visible_button_by_name(window, "Remove")
            if install_btn is not None:
                break
        if install_btn is None:
            raise RuntimeError("Execute button not found on VerifyReady dialog")
        activate(install_btn)
        consume_vs_prompt(timeout=1)

        if expect_vs_prompt:
            print(f"[pywinauto] Waiting for VS-running prompt -> {vs_prompt_action}")
            if not saw_vs_prompt and not handle_vs_prompt(vs_prompt_action, timeout=30):
                raise RuntimeError("Expected VS-running prompt did not appear")

        print("[pywinauto] Waiting for finish or error")
        end = time.time() + timeout_seconds
        while time.time() < end:
            try:
                window = wait_window(timeout=2)
            except RuntimeError:
                installer.poll()
                if installer.returncode == 0:
                    print("[pywinauto] PASS (installer exited successfully)")
                    return
                if installer.returncode is not None:
                    raise RuntimeError(f"Installer exited with code {installer.returncode}")
                time.sleep(0.25)
                continue

            if contains_text(window, "Error 2343"):
                raise RuntimeError("Detected MSI UI error: Error 2343")

            finish = find_visible(window, "Finish")
            if finish is None:
                finish = find_visible_button_by_name(window, "Finish")
            if finish is not None:
                activate(finish)
                print("[pywinauto] PASS")
                return

            time.sleep(0.25)

        raise RuntimeError("Installer did not reach Finish within timeout")
    finally:
        try:
            installer.poll()
            if installer.returncode is None:
                installer.terminate()
        except Exception:
            pass

        if not skip_uninstall:
            print("[pywinauto] Cleanup uninstall")
            subprocess.run(
                [
                    "msiexec",
                    "/x",
                    str(msi),
                    "ALLUSERS=2",
                    "MSIINSTALLPERUSER=1",
                    "/qn",
                ],
                check=False,
            )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msi", required=True)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--skip-uninstall", action="store_true")
    parser.add_argument("--msi-log", default="")
    parser.add_argument("--expect-vs-prompt", action="store_true")
    parser.add_argument("--vs-prompt-action", choices=["cancel", "retry"], default="cancel")
    parser.add_argument("--maintenance-action", choices=["change", "repair", "remove"], default="repair")
    args = parser.parse_args()

    try:
        msi_log = args.msi_log if args.msi_log else None
        run(args.msi, args.timeout, args.skip_uninstall, msi_log, args.expect_vs_prompt, args.vs_prompt_action, args.maintenance_action)
        return 0
    except Exception as exc:
        print(f"[pywinauto] FAIL: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
