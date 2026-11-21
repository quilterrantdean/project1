# ui_components.py
# -*- coding: utf-8 -*-

import datetime
import os
import config

try:
    import tkinter as tk
    from tkinter import ttk
except Exception:
    tk = None
    ttk = None

from gui_utils import center_window

_TkToplevelBase = tk.Toplevel if tk else object

class SilentDialog(_TkToplevelBase):
    def __init__(self, parent, title, message, is_error=False):
        if not tk or not ttk:
            return
        super(SilentDialog, self).__init__(parent)
        self.transient(parent)
        self.title(title)
        self.result = False
        self.resizable(False, False)
        try:
            if os.path.exists(config.ICON_FILE):
                self.iconbitmap(config.ICON_FILE)
        except Exception:
            pass

        frame = ttk.Frame(self, padding="20")
        frame.pack(expand=True, fill=tk.BOTH)

        if is_error:
            lbl_icon = tk.Label(frame, text="!", font=("Arial", 20, "bold"), fg="white", bg="#cc0000", width=3)
        else:
            lbl_icon = tk.Label(frame, text="i", font=("Arial", 20, "bold"), fg="white", bg="#0055cc", width=3)
        lbl_icon.pack(side=tk.LEFT, padx=(0, 15), anchor=tk.N)

        msg_lbl = ttk.Label(frame, text=message, wraplength=350, justify=tk.LEFT)
        msg_lbl.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        btn_frame = ttk.Frame(self, padding="10")
        btn_frame.pack(fill=tk.X, side=tk.BOTTOM)
        
        ok_btn = ttk.Button(btn_frame, text="OK", command=self.destroy)
        ok_btn.pack()
        
        center_window(self)
        self.grab_set()
        self.wait_window(self)

class ConfirmationDialog(_TkToplevelBase):
    def __init__(self, parent, title, message):
        if not tk or not ttk:
            return
        super(ConfirmationDialog, self).__init__(parent)
        self.transient(parent)
        self.title(title)
        self.result = False
        try:
            if os.path.exists(config.ICON_FILE):
                self.iconbitmap(config.ICON_FILE)
        except Exception: 
            pass

        frame = ttk.Frame(self, padding="20")
        frame.pack(expand=True, fill=tk.BOTH)

        lbl_icon = tk.Label(frame, text="?", font=("Arial", 20, "bold"), fg="white", bg="#d9a300", width=3)
        lbl_icon.pack(side=tk.LEFT, padx=(0, 15), anchor=tk.N)
        
        ttk.Label(frame, text=message, wraplength=350).pack(side=tk.LEFT)

        btn_frame = ttk.Frame(self, padding="10")
        btn_frame.pack(fill=tk.X, side=tk.BOTTOM)

        def on_yes():
            self.result = True
            self.destroy()
            
        def on_no():
            self.result = False
            self.destroy()

        by = ttk.Button(btn_frame, text="Sim", command=on_yes)
        by.pack(side=tk.LEFT, padx=20, expand=True)
        bn = ttk.Button(btn_frame, text="Não", command=on_no)
        bn.pack(side=tk.LEFT, padx=20, expand=True)
        
        center_window(self)
        self.grab_set()
        self.wait_window(self)

def show_silent_info(parent, title, msg):
    if tk and parent and parent.winfo_exists():
        SilentDialog(parent, title, msg, is_error=False)

def show_silent_error(parent, title, msg):
    if tk and parent and parent.winfo_exists():
        SilentDialog(parent, title, msg, is_error=True)

def ask_silent_yesno(parent, title, msg):
    if not tk or not parent or not parent.winfo_exists():
        return False
    d = ConfirmationDialog(parent, title, msg)
    return d.result

class NewRecordDialog(tk.Toplevel):
    def __init__(self, parent, callback, initial=None):
        if not tk:
            if callback: callback(None)
            return
        super(NewRecordDialog, self).__init__(parent)
        self.transient(parent)
        self.grab_set()
        self.callback = callback
        self.result = None
        self.initial = initial

        title_text = "Editar Registo" if initial else "Novo Registo"
        self.title(title_text)

        try:
            if os.path.exists(config.ICON_FILE):
                self.iconbitmap(config.ICON_FILE)
        except Exception:
            pass

        frame = ttk.Frame(self, padding="15")
        frame.pack(expand=True, fill=tk.BOTH)

        frame.columnconfigure(1, weight=1) 
        frame.columnconfigure(2, weight=0) 
        frame.columnconfigure(3, weight=0) 

        # --- Linha 0: Título ---
        ttk.Label(frame, text="Título:").grid(row=0, column=0, sticky=tk.W, padx=5, pady=5)
        self.title_entry = ttk.Entry(frame)
        self.title_entry.grid(row=0, column=1, columnspan=3, sticky=tk.EW, padx=5, pady=5)

        # --- Linha 1: Tipo ---
        ttk.Label(frame, text="Tipo:").grid(row=1, column=0, sticky=tk.W, padx=5, pady=5)
        self.type_var = tk.StringVar()
        self.type_combo = ttk.Combobox(frame, textvariable=self.type_var, values=["livro", "audiolivro"], state="readonly")
        self.type_combo.grid(row=1, column=1, sticky=tk.W, padx=5, pady=5)

        # --- Linha 2: Data e Hora ---
        ttk.Label(frame, text="Data (AAAA-MM-DD):").grid(row=2, column=0, sticky=tk.W, padx=5, pady=5)
        self.date_entry = ttk.Entry(frame, width=12)
        self.date_entry.grid(row=2, column=1, sticky=tk.W, padx=5, pady=5)

        ttk.Label(frame, text="Hora (HH:MM):").grid(row=2, column=2, sticky=tk.E, padx=5, pady=5)
        self.time_entry = ttk.Entry(frame, width=8)
        self.time_entry.grid(row=2, column=3, sticky=tk.W, padx=5, pady=5)

        # --- Botões ---
        btn_frame = ttk.Frame(frame)
        btn_frame.grid(row=3, column=0, columnspan=4, pady=(20, 0))

        add_text = "Guardar" if initial else "Adicionar"
        add_button = ttk.Button(btn_frame, text=add_text, command=self._on_add)
        add_button.pack(side=tk.LEFT, padx=10)
        
        cancel_button = ttk.Button(btn_frame, text="Cancelar", command=self._on_cancel)
        cancel_button.pack(side=tk.LEFT, padx=10)

        self.protocol("WM_DELETE_WINDOW", self._on_cancel)
        self.bind("<Return>", lambda e: self._on_add())
        self.bind("<Escape>", lambda e: self._on_cancel())

        now = datetime.datetime.now()
        default_date = now.strftime(config.DATE_ONLY_FORMAT)
        default_time = now.strftime(config.TIME_ONLY_FORMAT)
        default_type = "livro"
        default_title = ""

        if initial:
            default_title = initial.get('title', '')
            ld = initial.get('loan_date') 
            default_type = initial.get('type', 'livro')
            if ld:
                if isinstance(ld, datetime.datetime):
                    default_date = ld.strftime(config.DATE_ONLY_FORMAT)
                    default_time = ld.strftime(config.TIME_ONLY_FORMAT)
                else:
                    default_date = ld.strftime(config.DATE_ONLY_FORMAT)
                    default_time = now.strftime(config.TIME_ONLY_FORMAT)
        
        self.title_entry.insert(0, default_title)
        self.type_combo.set(default_type)
        self.date_entry.insert(0, default_date)
        self.time_entry.insert(0, default_time)
        
        if not initial:
            self.title_entry.focus_set()
            self.geometry("500x220")

        center_window(self)
        self.wait_window(self)

    def _on_add(self):
        title = (self.title_entry.get() or '').strip()
        loan_type = (self.type_var.get() or '').strip()
        date_str = (self.date_entry.get() or '').strip()
        time_str = (self.time_entry.get() or '').strip()

        if not title:
            show_silent_error(self, "Erro", "O título é obrigatório.")
            self.title_entry.focus_set()
            return
        
        if not loan_type:
            show_silent_error(self, "Erro", "Tipo de empréstimo obrigatório.")
            self.type_combo.focus_set()
            return
        
        try:
            dt_date = datetime.datetime.strptime(date_str, config.DATE_ONLY_FORMAT).date()
        except ValueError:
            show_silent_error(self, "Erro", "Data inválida. Use o formato AAAA-MM-DD.")
            self.date_entry.focus_set()
            return
        
        try:
            dt_time = datetime.datetime.strptime(time_str, config.TIME_ONLY_FORMAT).time()
        except ValueError:
            show_silent_error(self, "Erro", "Hora inválida. Use o formato HH:MM.")
            self.time_entry.focus_set()
            return

        full_dt = datetime.datetime.combine(dt_date, dt_time)
        self.result = {'title': title, 'loan_date': full_dt, 'type': loan_type}
        
        cb = self.callback
        self.destroy() 
        
        if cb:
            cb(self.result)

    def _on_cancel(self):
        self.result = None
        if self.callback:
            self.callback(None)
        self.destroy()
