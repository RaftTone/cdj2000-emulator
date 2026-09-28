"""Small, host-only checks for the agent-oriented NXS launcher affordances."""
import json
import pytest

from tools.cdj_main import nxs_vm


FRAME = b"P6\n2 1\n255\n\0\xff\x80\xff\0\0"


def test_automatic_run_path_avoids_timestamp_collision(tmp_path, monkeypatch):
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm.time, 'strftime',
                        lambda format_string, clock: '20260915-120000')
    (tmp_path / 'runs' / 'nxs-20260915-120000').mkdir(parents=True)
    assert nxs_vm.automatic_run_path() == \
        tmp_path / 'runs' / 'nxs-20260915-120000-1'


def test_occupied_ports_are_reported_before_run_creation(tmp_path, monkeypatch):
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm, 'occupied_local_ports', lambda base, debug: [base + 4])
    monkeypatch.setattr(nxs_vm.sys, 'argv', ['nxs_vm', 'run', '--port', '6200'])
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2
    assert not (tmp_path / 'run').exists()


def test_occupied_port_probe_binds_without_connecting(monkeypatch):
    calls = []

    class Probe:
        def settimeout(self, value):
            calls.append(('timeout', value))
        def bind(self, address):
            calls.append(('bind', address))
            if address[1] == 6202:
                raise OSError('in use')
        def close(self):
            calls.append(('close',))

    monkeypatch.setattr(nxs_vm.socket, 'socket', lambda *args: Probe())
    assert nxs_vm.occupied_local_ports(6200, False) == [6202]
    assert not any(call[0] == 'connect_ex' for call in calls)
    assert ('timeout', 0.25) in calls


def test_gui_board_override_points_cfi_at_selected_flash(tmp_path):
    template = tmp_path / 'stock.hw'
    template.write_text('/core/bfin_ebiu_amc/cfi@0/file "firmware/nxs/gui-flash-image.bin"\n')
    flash = tmp_path / 'dev' / 'gui-flash-image.bin'
    flash.parent.mkdir()
    flash.write_bytes(b'flash')
    output = tmp_path / 'run' / 'gui-board.hw'
    output.parent.mkdir()
    nxs_vm.write_gui_board_override(template, flash, output)
    assert f'/core/bfin_ebiu_amc/cfi@0/file "{flash.resolve().as_posix()}"' == output.read_text().strip()


def test_gui_firmware_override_launches_with_generated_board_and_records_it(
        tmp_path, monkeypatch):
    for name in ('bin/cdj-run', 'build/qemu/build/qemu-system-sh4',
                 'firmware/nxs/main-firmware.bin'):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    stock_board = tmp_path / 'emulator/cdj2000-gui-nxs.hw'
    stock_board.parent.mkdir()
    stock_board.write_text('/core/bfin_ebiu_amc/cfi@0/file "stock.bin"\n')
    gui = tmp_path / 'development-gui'
    gui.mkdir()
    (gui / 'gui-boot-memory.elf').write_bytes(b'elf')
    (gui / 'gui-flash-image.bin').write_bytes(b'flash')
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm, 'occupied_local_ports', lambda base, debug: [])
    monkeypatch.setattr(nxs_vm.sys, 'argv', [
        'nxs_vm', 'run', '--seconds', '1', '--lightweight',
        '--gui-firmware', str(gui)])

    class Process:
        def __init__(self, is_gui):
            self.is_gui, self.stopped = is_gui, False
            self.pid = 123
        def poll(self):
            return 0 if self.is_gui or self.stopped else None
        def terminate(self):
            self.stopped = True
        def wait(self, timeout):
            return 0

    commands = []
    environments = []
    monkeypatch.setenv('CDJ_NXS_DSP_EVENTS', '/inherited/events.jsonl')
    def launch(command, **kwargs):
        commands.append(command)
        environments.append(kwargs['env'])
        is_gui = '--model' in command
        if is_gui:
            (tmp_path / 'run/screen.ppm').write_bytes(FRAME)
        return Process(is_gui)
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', launch)

    assert nxs_vm.main() == 0
    gui_command = next(command for command in commands if '--model' in command)
    board = tmp_path / 'run/gui-board.hw'
    assert gui_command[gui_command.index('--hw-board-file') + 1] == str(board)
    assert f'"{(gui / "gui-flash-image.bin").resolve().as_posix()}"' in board.read_text()
    manifest = json.loads((tmp_path / 'run/run.json').read_text())
    assert manifest['firmware']['gui_board_file'] == str(board)
    assert manifest['firmware']['gui_board_mode'] == 'run-local override'
    assert 'run-local copy' in manifest['input_provenance_notes']
    assert manifest['dsp_capture_enabled'] is True
    assert manifest['dsp_capture_mode'] == 'fault-only'
    assert manifest['dsp_event_capture_enabled'] is False
    assert manifest['dsp_checkpoint_policy'] == 'fault'
    assert manifest['architectural_validation_eligible'] is False
    main_env = environments[0]
    assert main_env['CDJ_NXS_DSP_CHECKPOINT_POLICY'] == 'fault'
    assert main_env['CDJ_NXS_DSP_CHECKPOINT_DIR'] == str(tmp_path / 'run/dsp-checkpoints')
    assert 'CDJ_NXS_DSP_EVENTS' not in main_env


def test_stop_request_cleanly_stops_owned_processes(tmp_path, monkeypatch):
    for name in ('bin/cdj-run', 'build/qemu/build/qemu-system-sh4',
                 'firmware/nxs/main-firmware.bin',
                 'firmware/nxs/gui-boot-memory.elf',
                 'firmware/nxs/gui-flash-image.bin'):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm, 'occupied_local_ports', lambda base, debug: [])
    monkeypatch.setattr(nxs_vm.sys, 'argv', [
        'nxs_vm', 'run', '--seconds', '60', '--lightweight'])
    sleep_count = [0]
    def sleep(duration):
        sleep_count[0] += 1
        if sleep_count[0] == 2:
            (tmp_path / 'run/stop-request.json').write_text('{}')
    monkeypatch.setattr(nxs_vm.time, 'sleep', sleep)

    class Process:
        def __init__(self):
            self.pid = 123
            self.stopped = False
            self.terminate_calls = 0
        def poll(self):
            return 0 if self.stopped else None
        def terminate(self):
            self.terminate_calls += 1
            self.stopped = True
        def wait(self, timeout):
            return 0

    processes = []
    def launch(command, **kwargs):
        process = Process()
        processes.append(process)
        return process
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', launch)

    assert nxs_vm.main() == 0
    result = json.loads((tmp_path / 'run/result.json').read_text())
    session = json.loads((tmp_path / 'run/session.json').read_text())
    assert result['stop_requested'] is True
    assert result['timed_out'] is False
    assert session['state'] == 'stopped'
    assert len(processes) == 2
    assert all(process.terminate_calls == 1 for process in processes)


@pytest.mark.parametrize('failure', [
    {'gui_exit': 1}, {'main_exit_before_teardown': 1},
    {'finalization_error': 'capture incomplete'}, {'error': 'viewer failed'},
    {'timed_out': True},
])
def test_stop_request_does_not_mask_concurrent_failure(failure):
    assert not nxs_vm.run_succeeded({'stop_requested': True, **failure})


def test_test_track_and_sd_conflict_before_inputs_or_launch(tmp_path, monkeypatch):
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm, 'occupied_local_ports', lambda base, debug: [])
    monkeypatch.setattr(nxs_vm.sys, 'argv', [
        'nxs_vm', 'run', '--test-track', '--sd', 'card.img'])
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2
    assert not (tmp_path / 'run').exists()


@pytest.mark.parametrize('extra, message', [
    (['--source-key', 'bad-key'], '--source-key must be'),
    (['--source-key', '22:01'], '--source-key must be'),
    (['--source-key-at', 'nan'], '--source-key-at must be finite'),
    (['--source-key-at', 'inf'], '--source-key-at must be finite'),
])
def test_invalid_source_options_leave_run_name_available(
        tmp_path, monkeypatch, capsys, extra, message):
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm.sys, 'argv', ['nxs_vm', 'run', *extra])
    with pytest.raises(SystemExit) as error:
        nxs_vm.main()
    assert error.value.code == 2
    assert message in capsys.readouterr().err
    assert not (tmp_path / 'run').exists()


def test_agent_commands_include_status_report_panel_and_debug(capsys, tmp_path, monkeypatch):
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    nxs_vm.print_agent_commands(tmp_path / 'runs' / 'dev', 6200, True)
    output = capsys.readouterr().out
    assert 'run: runs/dev' in output
    assert 'run_state runs/dev' in output
    assert 'run_report runs/dev' in output
    assert 'tools.cdj_main.dev runs/dev status --json' in output
    assert 'panel_control --port 6204 state' in output
    assert 'gdb: target remote 127.0.0.1:6203' in output
    assert 'qmp.sock' in output


def test_debug_chardev_stays_unix_on_posix_and_tcp_on_windows(tmp_path, monkeypatch):
    for name in ('bin/cdj-run', 'build/qemu/build/qemu-system-sh4',
                 'firmware/nxs/main-firmware.bin',
                 'firmware/nxs/gui-boot-memory.elf',
                 'firmware/nxs/gui-flash-image.bin'):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode())
    monkeypatch.setattr(nxs_vm, 'ROOT', tmp_path)
    monkeypatch.setattr(nxs_vm, 'occupied_local_ports', lambda base, debug: [])
    monkeypatch.setattr(nxs_vm.sys, 'argv', [
        'nxs_vm', 'run', '--seconds', '1', '--lightweight', '--debug',
        '--qemu-sync-profile'])
    commands = []

    class Process:
        def __init__(self, is_gui):
            self.is_gui, self.stopped, self.pid = is_gui, False, 123
        def poll(self):
            return 0 if self.is_gui or self.stopped else None
        def terminate(self):
            self.stopped = True
        def wait(self, timeout):
            return 0

    def launch(command, **kwargs):
        commands.append(command)
        if '--model' in command:
            (tmp_path / 'run/screen.ppm').write_bytes(FRAME)
        return Process('--model' in command)
    monkeypatch.setattr(nxs_vm.subprocess, 'Popen', launch)
    monkeypatch.setattr(nxs_vm, 'capture_sync_profile', lambda *a, **k: {'status': 'captured'})
    assert nxs_vm.main() == 0
    main = next(command for command in commands if '-M' in command)
    if nxs_vm.UNIX_CONTROL:
        assert any(part.startswith('unix:') and 'qmp.sock' in part for part in main)
        assert any(part.startswith('unix:') and 'qemu-monitor.sock' in part for part in main)
        assert not any(part.startswith('tcp:') and 'server=on' in part for part in main)
    else:
        assert any(part.startswith('tcp:127.0.0.1:') and 'server=on' in part for part in main)
        assert any(part.startswith('telnet:127.0.0.1:') for part in main)
        assert not any(part.startswith('unix:') for part in main)
