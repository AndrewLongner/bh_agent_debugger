import argparse
import subprocess
import sys
import time

import verify_installer_ui_pywinauto as ui


def find_execute_button():
    for window in ui.all_setup_windows():
        for auto_id in ("Install", "InstallNoShield", "Repair", "Remove", "Change", "Update"):
            control = ui.find_visible(window, auto_id)
            if control is not None:
                return control

        for label in ("Install", "Repair", "Remove", "Change", "Update"):
            control = ui.find_visible_button_by_name(window, label)
            if control is not None:
                return control

    return None


def navigate_to_execute(maintenance_action):
    ui.wait_window(30)
    ui.click("Next", "Next")

    mode = ui.detect_flow_mode(20)
    print(f"[detect] mode={mode}")

    if mode == "maintenance":
        ui.click(maintenance_action.title(), maintenance_action)
        return

    ui.set_checked("LicenseAcceptedCheckBox")
    ui.click("Next", "Next")
    ui.click("Next", "Next")
    ui.click("Next", "Next")
    ui.click("ChangeFolder", "Change")
    ui.click("OK", "OK")
    ui.click("Next", "Next")


def main():
    parser = argparse.ArgumentParser(description="Detect Visual Studio running popup in installer UI")
    parser.add_argument("--msi", required=True)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--maintenance-action", choices=["change", "repair", "remove"], default="repair")
    parser.add_argument("--dismiss", action="store_true", help="Click Cancel on popup if found")
    parser.add_argument("--spawn-vs", action="store_true", help="Launch devenv before running test")
    parser.add_argument("--devenv-path", default=r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.exe")
    parser.add_argument("--quiet", action="store_true", help="Run MSI in /qn mode and only detect popup occurrence")
    parser.add_argument("--machine", action="store_true", help="Use ALLUSERS=1 for quiet install")
    parser.add_argument("--msi-log", default="", help="Optional MSI log path")
    args = parser.parse_args()

    vs_proc = None
    installer = None

    try:
        if args.spawn_vs:
            vs_proc = subprocess.Popen([args.devenv_path])
            time.sleep(8)

        if args.quiet:
            cmd = ["msiexec", "/i", args.msi]
            if args.machine:
                cmd.append("ALLUSERS=1")
            if args.msi_log:
                cmd.extend(["/l*v", args.msi_log])
            cmd.append("/qn")
            installer = subprocess.Popen(cmd)

            popup_detected = False
            end = time.time() + args.timeout
            while time.time() < end:
                if ui.find_vs_prompt_window() is not None:
                    popup_detected = True
                    try:
                        ui.handle_vs_prompt("cancel", timeout=2)
                    except Exception:
                        pass
                    break
                if installer.poll() is not None:
                    break
                time.sleep(0.2)

            if popup_detected and installer.poll() is None:
                installer.terminate()
                try:
                    subprocess.run(["cmd", "/c", "taskkill /F /IM msiexec.exe"], check=False)
                except Exception:
                    pass

            installer.wait(timeout=max(args.timeout, 120))
            print(f"MSI_EXIT={installer.returncode}")
            print(f"POPUP_DETECTED={1 if popup_detected else 0}")
            return 1 if popup_detected else 0

        installer = subprocess.Popen(["msiexec", "/i", args.msi])

        navigate_to_execute(args.maintenance_action)

        button = find_execute_button()
        if button is None:
            raise RuntimeError("Execute button not found")
        ui.activate(button)

        popup = ui.find_vs_prompt_window()
        if popup is None:
            end = time.time() + args.timeout
            while time.time() < end:
                popup = ui.find_vs_prompt_window()
                if popup is not None:
                    break
                time.sleep(0.2)

        if popup is None:
            print("POPUP_DETECTED=0")
            return 1

        print("POPUP_DETECTED=1")

        if args.dismiss:
            if not ui.handle_vs_prompt("cancel", timeout=5):
                raise RuntimeError("Popup found but could not dismiss it")

        return 0
    except Exception as exc:
        print(f"ERROR={exc}")
        return 2
    finally:
        if installer is not None and installer.poll() is None:
            installer.terminate()
        if vs_proc is not None and vs_proc.poll() is None:
            vs_proc.terminate()


if __name__ == "__main__":
    raise SystemExit(main())
