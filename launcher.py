#!/usr/bin/env python3
"""Minimal vanilla launcher for the CUSA01623 1.02 compatibility runtime."""
from __future__ import annotations

import os
import json
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

ROOT = Path(__file__).resolve().parent
SETTINGS_FILE = ROOT / 'launcher_settings.json'
RESOLUTIONS = ('1280x720', '1920x1080')
sys.path.insert(0, str(ROOT / 'scripts'))
import game_check  # noqa: E402
import patches as gow_patches  # noqa: E402


def load_settings(path: Path = SETTINGS_FILE):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def launch_options(resolution: str, fullscreen: bool, skip_intro: bool, skip_video: bool,
                   video_patch: str = gow_patches.TEXTURE_FIX, fps_120: bool = False):
    """Translate launcher controls to the runtime's stable environment interface."""
    if resolution not in RESOLUTIONS:
        resolution = '1920x1080'
    width, height = resolution.split('x')
    if video_patch not in (*gow_patches.VIDEO_PROFILES, gow_patches.NO_VIDEO_PATCH):
        raise ValueError(f'Unknown video patch: {video_patch}')
    selected = [] if video_patch == gow_patches.NO_VIDEO_PATCH else [video_patch]
    if fps_120:
        selected.append(gow_patches.FPS_120)
    options = {
        'GOW3R_INTERNAL_WIDTH': width,
        'GOW3R_INTERNAL_HEIGHT': height,
        'GOW3R_FULLSCREEN': '1' if fullscreen else '0',
        'GOW3R_PATCH_SKIP_INTRO': '1' if skip_intro else '0',
        'GOW3R_PATCH_SKIP_VIDEO': '1' if skip_video else '0',
        'GOW3R_PATCHES': ';'.join(selected) or 'none',
        'GOW3R_VBLANK_HZ': '60',
        'GOW3R_FPS_LIMIT': '60',
        # Precise readbacks are required for the vegetation/textures on Gaia.
        # Keep explicit environment overrides available for diagnostic runs.
        'GOW3R_READBACKS': os.environ.get('GOW3R_READBACKS', '2'),
        'GOW3R_READBACK_LINEAR': os.environ.get('GOW3R_READBACK_LINEAR', '1'),
        'GOW3R_DIRECT_MEMORY_ACCESS': os.environ.get('GOW3R_DIRECT_MEMORY_ACCESS', '0'),
    }
    options.update(gow_patches.runtime_options(selected))
    return options


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('GoW3R Windows Port')
        self.geometry('860x690')
        self.minsize(780, 640)
        saved = load_settings()
        default_game = ROOT.parent / 'CUSA01623 GoW3Remaster 1.02'
        self.game = tk.StringVar(value=os.environ.get(
            'GOW3R_GAME_DIR', saved.get('game', str(default_game))))
        self.user = tk.StringVar(value=os.environ.get(
            'GOW3R_USER_DIR', saved.get('user', str(ROOT / 'user' / 'CUSA01623'))))
        env_resolution = (f"{os.environ.get('GOW3R_INTERNAL_WIDTH', '')}x"
                          f"{os.environ.get('GOW3R_INTERNAL_HEIGHT', '')}")
        resolution = env_resolution if env_resolution in RESOLUTIONS else saved.get(
            'resolution', '1920x1080')
        self.resolution = tk.StringVar(
            value=resolution if resolution in RESOLUTIONS else '1920x1080')
        self.fullscreen = tk.BooleanVar(value=os.environ.get(
            'GOW3R_FULLSCREEN', str(int(bool(saved.get('fullscreen', False))))) == '1')
        self.skip_intro = tk.BooleanVar(value=os.environ.get(
            'GOW3R_PATCH_SKIP_INTRO', str(int(bool(saved.get('skip_intro', False))))) == '1')
        self.skip_video = tk.BooleanVar(value=os.environ.get(
            'GOW3R_PATCH_SKIP_VIDEO', str(int(bool(saved.get('skip_video', False))))) == '1')
        self.patch_catalog = gow_patches.catalog()
        self.video_patch_names = (gow_patches.NO_VIDEO_PATCH,) + tuple(
            name for name in gow_patches.VIDEO_PROFILES if name in self.patch_catalog)
        selected_env = os.environ.get('GOW3R_PATCHES', '').split(';')
        profile = next((name for name in selected_env if name in self.video_patch_names),
                       saved.get('video_patch', gow_patches.TEXTURE_FIX))
        if selected_env == ['none']:
            profile = gow_patches.NO_VIDEO_PATCH
        self.video_patch = tk.StringVar(value=profile if profile in self.video_patch_names
                                        else gow_patches.TEXTURE_FIX)
        self.fps_120 = tk.BooleanVar(value=(gow_patches.FPS_120 in selected_env
                                    if 'GOW3R_PATCHES' in os.environ
                                    else bool(saved.get('fps_120', False))))
        self.patch_summary = tk.StringVar()
        self.status = tk.StringVar(value='Select and validate your decrypted game dump.')
        self.process: subprocess.Popen | None = None
        self._build_ui()
        self.video_patch.trace_add('write', self.update_patch_controls)
        self.update_patch_controls()

    def _build_ui(self):
        frame = ttk.Frame(self, padding=18)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='God of War III Remastered — Windows compatibility runtime',
                  font=('Segoe UI', 14, 'bold')).grid(row=0, column=0, columnspan=3,
                                                     sticky='w', pady=(0, 18))
        ttk.Label(frame, text='Game folder').grid(row=1, column=0, sticky='w')
        ttk.Entry(frame, textvariable=self.game).grid(row=2, column=0, columnspan=2,
                                                     sticky='ew', padx=(0, 8))
        ttk.Button(frame, text='Browse…', command=self.pick_game).grid(row=2, column=2)
        ttk.Label(frame, text='User data / saves').grid(row=3, column=0,
                                                               sticky='w', pady=(14, 0))
        ttk.Entry(frame, textvariable=self.user).grid(row=4, column=0, columnspan=2,
                                                     sticky='ew', padx=(0, 8))
        ttk.Button(frame, text='Browse…', command=self.pick_user).grid(row=4, column=2)
        options = ttk.Frame(frame)
        options.grid(row=5, column=0, columnspan=3, sticky='ew', pady=(18, 4))
        patches = ttk.LabelFrame(options, text='Optional patches', padding=10)
        patches.pack(side='left', fill='both', expand=True, padx=(0, 8))
        ttk.Label(patches, text='Texture / game resolution (select one)').pack(anchor='w')
        ttk.Combobox(patches, textvariable=self.video_patch, values=self.video_patch_names,
                     state='readonly', width=43).pack(fill='x', pady=(4, 6))
        ttk.Label(patches, textvariable=self.patch_summary, wraplength=430).pack(anchor='w')
        ttk.Button(patches, text='Patch details', command=self.show_patch_details).pack(
            anchor='w', pady=(6, 8))
        ttk.Checkbutton(patches, text='Skip intro', variable=self.skip_intro).pack(
            anchor='w')
        ttk.Checkbutton(patches, text='Skip any video with the X button',
                        variable=self.skip_video).pack(anchor='w', pady=(6, 0))
        ttk.Checkbutton(patches, text='120 FPS (VBlank 120 Hz)', variable=self.fps_120).pack(
            anchor='w', pady=(6, 0))
        video = ttk.LabelFrame(options, text='Video', padding=10)
        video.pack(side='left', fill='both', expand=True)
        ttk.Label(video, text='Renderer resolution').grid(row=0, column=0, sticky='w')
        self.resolution_combo = ttk.Combobox(video, textvariable=self.resolution,
                                             values=RESOLUTIONS, state='readonly', width=14)
        self.resolution_combo.grid(row=1, column=0, sticky='w', pady=(4, 0))
        ttk.Checkbutton(video, text='Fullscreen', variable=self.fullscreen).grid(
            row=2, column=0, columnspan=2, sticky='w', pady=(8, 0))
        actions = ttk.Frame(frame)
        actions.grid(row=6, column=0, columnspan=3, sticky='w', pady=14)
        ttk.Button(actions, text='Validate', command=self.validate).pack(side='left')
        self.launch_button = ttk.Button(actions, text='Prepare and launch', command=self.launch)
        self.launch_button.pack(side='left', padx=8)
        ttk.Button(actions, text='Open log', command=lambda: self.open_path(ROOT / 'gow3r.log')).pack(side='left')
        ttk.Button(actions, text='Open reports', command=lambda: self.open_path(ROOT / 'out')).pack(side='left', padx=8)
        ttk.Separator(frame).grid(row=7, column=0, columnspan=3, sticky='ew')
        ttk.Label(frame, textvariable=self.status, wraplength=690).grid(
            row=8, column=0, columnspan=3, sticky='w', pady=(14, 8))
        self.output = tk.Text(frame, height=9, state='disabled', wrap='word')
        self.output.grid(row=9, column=0, columnspan=3, sticky='nsew')
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(9, weight=1)

    def update_patch_controls(self, *_):
        name = self.video_patch.get()
        resolution_patch = name.startswith('Resolution Patch - ')
        if name in gow_patches.VIDEO_PROFILES:
            width, height, additional = gow_patches.VIDEO_PROFILES[name]
            if resolution_patch:
                output = f'{width}x{height}'
                self.resolution_combo.configure(values=(output,), state='disabled')
                self.resolution.set(output)
            self.patch_summary.set(
                f'Game output: {width}×{height}. Total direct memory: at least '
                f'{5056 + additional} MiB ({additional} MiB extra), adjusted automatically.')
        else:
            self.patch_summary.set('Original Video Arena and game resolution; no graphics patch.')
        if not resolution_patch:
            self.resolution_combo.configure(values=RESOLUTIONS, state='readonly')
            if self.resolution.get() not in RESOLUTIONS:
                self.resolution.set('1920x1080')

    def show_patch_details(self):
        names = [self.video_patch.get()]
        if self.fps_120.get():
            names.append(gow_patches.FPS_120)
        details = []
        for name in names:
            metadata = self.patch_catalog.get(name)
            if metadata:
                details.append(f"{name}\nAuthor: {metadata.get('Author', '')}\n"
                               f"{metadata.get('Note', '')}")
        messagebox.showinfo('Patches — CUSA01623 01.02', '\n\n'.join(details)
                            or 'No graphics patch selected.')

    def save_settings(self):
        data = {
            'game': self.game.get(),
            'user': self.user.get(),
            'resolution': self.resolution.get(),
            'fullscreen': self.fullscreen.get(),
            'skip_intro': self.skip_intro.get(),
            'skip_video': self.skip_video.get(),
            'video_patch': self.video_patch.get(),
            'fps_120': self.fps_120.get(),
        }
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n',
                                     encoding='utf-8')
        except OSError as error:
            self.write(f'Could not save launcher settings: {error}\n')

    def pick_game(self):
        path = filedialog.askdirectory(title='Game folder containing eboot.bin',
                                       initialdir=self.game.get() or ROOT.parent)
        if path:
            self.game.set(path)
            self.validate()

    def pick_user(self):
        path = filedialog.askdirectory(title='User data folder',
                                       initialdir=self.user.get() or ROOT)
        if path:
            self.user.set(path)

    def write(self, text):
        self.output.configure(state='normal')
        self.output.insert('end', text)
        self.output.see('end')
        self.output.configure(state='disabled')

    def validate(self):
        self.status.set('Calculating loadable image hash…')
        self.update_idletasks()
        result = game_check.inspect(Path(self.game.get()))
        if result['ok']:
            self.status.set(f"Valid: CUSA01623 01.02 — {result['image_sha256']}")
            return True
        message = game_check.explain(result['kind'], result['title_id'], result['app_version'])
        self.status.set(message)
        return False

    def launch(self):
        if self.process and self.process.poll() is None:
            messagebox.showinfo('GoW3R', 'The runtime is already running.')
            return
        if not self.validate():
            messagebox.showerror('Incompatible game dump', self.status.get())
            return
        try:
            options = launch_options(self.resolution.get(), self.fullscreen.get(),
                                     self.skip_intro.get(), self.skip_video.get(),
                                     self.video_patch.get(), self.fps_120.get())
        except ValueError as error:
            messagebox.showerror('Incompatible patches', str(error))
            return
        Path(self.user.get()).mkdir(parents=True, exist_ok=True)
        self.save_settings()
        env = os.environ.copy()
        env.update(GOW3R_GAME_DIR=self.game.get(), GOW3R_USER_DIR=self.user.get(),
                   GOW3R_DATA_DIR=str(ROOT), GOW3R_CONFIG=str(ROOT / 'gow3r.ini'),
                   PYTHON=sys.executable, PYTHONUTF8='1', PYTHONIOENCODING='utf-8')
        env.update(options)
        self.launch_button.configure(state='disabled')
        self.write('\nPreparing image and checking runtime contracts…\n')
        self.process = subprocess.Popen([str(ROOT / 'run.bat'), self.game.get()], cwd=ROOT,
                                        env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True,
                                        encoding='utf-8', errors='replace')
        threading.Thread(target=self._collect, daemon=True).start()

    def _collect(self):
        assert self.process and self.process.stdout
        for line in self.process.stdout:
            self.after(0, self.write, line)
        code = self.process.wait()
        self.after(0, self.status.set, f'Runtime exited with code {code}.')
        self.after(0, self.launch_button.configure, {'state': 'normal'})

    @staticmethod
    def open_path(path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix and not path.exists():
            path.touch()
        if not path.suffix:
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(path)


if __name__ == '__main__':
    Launcher().mainloop()
