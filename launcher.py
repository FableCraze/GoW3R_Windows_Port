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


def load_settings(path: Path = SETTINGS_FILE):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def launch_options(resolution: str, fullscreen: bool, skip_intro: bool, skip_video: bool):
    """Translate launcher controls to the runtime's stable environment interface."""
    if resolution not in RESOLUTIONS:
        resolution = '1920x1080'
    width, height = resolution.split('x')
    return {
        'GOW3R_INTERNAL_WIDTH': width,
        'GOW3R_INTERNAL_HEIGHT': height,
        'GOW3R_FULLSCREEN': '1' if fullscreen else '0',
        'GOW3R_PATCH_SKIP_INTRO': '1' if skip_intro else '0',
        'GOW3R_PATCH_SKIP_VIDEO': '1' if skip_video else '0',
    }


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('GoW3R Windows Port')
        self.geometry('760x570')
        self.minsize(680, 530)
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
        self.status = tk.StringVar(value='Selecione e valide o dump descriptografado.')
        self.process: subprocess.Popen | None = None
        self._build_ui()

    def _build_ui(self):
        frame = ttk.Frame(self, padding=18)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='God of War III Remastered — Windows compatibility runtime',
                  font=('Segoe UI', 14, 'bold')).grid(row=0, column=0, columnspan=3,
                                                     sticky='w', pady=(0, 18))
        ttk.Label(frame, text='Pasta do jogo').grid(row=1, column=0, sticky='w')
        ttk.Entry(frame, textvariable=self.game).grid(row=2, column=0, columnspan=2,
                                                     sticky='ew', padx=(0, 8))
        ttk.Button(frame, text='Selecionar…', command=self.pick_game).grid(row=2, column=2)
        ttk.Label(frame, text='Dados do usuário / saves').grid(row=3, column=0,
                                                               sticky='w', pady=(14, 0))
        ttk.Entry(frame, textvariable=self.user).grid(row=4, column=0, columnspan=2,
                                                     sticky='ew', padx=(0, 8))
        ttk.Button(frame, text='Selecionar…', command=self.pick_user).grid(row=4, column=2)
        options = ttk.Frame(frame)
        options.grid(row=5, column=0, columnspan=3, sticky='ew', pady=(18, 4))
        patches = ttk.LabelFrame(options, text='Patches opcionais', padding=10)
        patches.pack(side='left', fill='both', expand=True, padx=(0, 8))
        ttk.Checkbutton(patches, text='Pular introdução', variable=self.skip_intro).pack(
            anchor='w')
        ttk.Checkbutton(patches, text='Pular qualquer vídeo com o botão X',
                        variable=self.skip_video).pack(anchor='w', pady=(6, 0))
        video = ttk.LabelFrame(options, text='Vídeo', padding=10)
        video.pack(side='left', fill='both', expand=True)
        ttk.Label(video, text='Resolução').grid(row=0, column=0, sticky='w', padx=(0, 8))
        ttk.Combobox(video, textvariable=self.resolution, values=RESOLUTIONS,
                     state='readonly', width=14).grid(row=0, column=1, sticky='w')
        ttk.Checkbutton(video, text='Tela cheia', variable=self.fullscreen).grid(
            row=1, column=0, columnspan=2, sticky='w', pady=(8, 0))
        actions = ttk.Frame(frame)
        actions.grid(row=6, column=0, columnspan=3, sticky='w', pady=14)
        ttk.Button(actions, text='Validar', command=self.validate).pack(side='left')
        self.launch_button = ttk.Button(actions, text='Preparar e iniciar', command=self.launch)
        self.launch_button.pack(side='left', padx=8)
        ttk.Button(actions, text='Abrir log', command=lambda: self.open_path(ROOT / 'gow3r.log')).pack(side='left')
        ttk.Button(actions, text='Abrir relatórios', command=lambda: self.open_path(ROOT / 'out')).pack(side='left', padx=8)
        ttk.Separator(frame).grid(row=7, column=0, columnspan=3, sticky='ew')
        ttk.Label(frame, textvariable=self.status, wraplength=690).grid(
            row=8, column=0, columnspan=3, sticky='w', pady=(14, 8))
        self.output = tk.Text(frame, height=9, state='disabled', wrap='word')
        self.output.grid(row=9, column=0, columnspan=3, sticky='nsew')
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(9, weight=1)

    def save_settings(self):
        data = {
            'game': self.game.get(),
            'user': self.user.get(),
            'resolution': self.resolution.get(),
            'fullscreen': self.fullscreen.get(),
            'skip_intro': self.skip_intro.get(),
            'skip_video': self.skip_video.get(),
        }
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n',
                                     encoding='utf-8')
        except OSError as error:
            self.write(f'Não foi possível salvar as opções do launcher: {error}\n')

    def pick_game(self):
        path = filedialog.askdirectory(title='Pasta que contém eboot.bin',
                                       initialdir=self.game.get() or ROOT.parent)
        if path:
            self.game.set(path)
            self.validate()

    def pick_user(self):
        path = filedialog.askdirectory(title='Pasta de dados do usuário',
                                       initialdir=self.user.get() or ROOT)
        if path:
            self.user.set(path)

    def write(self, text):
        self.output.configure(state='normal')
        self.output.insert('end', text)
        self.output.see('end')
        self.output.configure(state='disabled')

    def validate(self):
        self.status.set('Calculando hash da imagem carregável…')
        self.update_idletasks()
        result = game_check.inspect(Path(self.game.get()))
        if result['ok']:
            self.status.set(f"Válido: CUSA01623 01.02 — {result['image_sha256']}")
            return True
        message = game_check.explain(result['kind'], result['title_id'], result['app_version'])
        self.status.set(message)
        return False

    def launch(self):
        if self.process and self.process.poll() is None:
            messagebox.showinfo('GoW3R', 'O runtime já está em execução.')
            return
        if not self.validate():
            messagebox.showerror('Dump incompatível', self.status.get())
            return
        Path(self.user.get()).mkdir(parents=True, exist_ok=True)
        self.save_settings()
        env = os.environ.copy()
        env.update(GOW3R_GAME_DIR=self.game.get(), GOW3R_USER_DIR=self.user.get(),
                   GOW3R_DATA_DIR=str(ROOT), GOW3R_CONFIG=str(ROOT / 'gow3r.ini'),
                   PYTHON=sys.executable, PYTHONUTF8='1', PYTHONIOENCODING='utf-8')
        env.update(launch_options(self.resolution.get(), self.fullscreen.get(),
                                  self.skip_intro.get(), self.skip_video.get()))
        self.launch_button.configure(state='disabled')
        self.write('\nPreparando imagem e verificando contratos…\n')
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
        self.after(0, self.status.set, f'Runtime encerrado com código {code}.')
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
