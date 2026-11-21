# main.py
# -*- coding: utf-8 -*-
# Aplicação principal - Gestor de Empréstimos Pessoal

import datetime
import os
import sys

import config
import data_utils
import gui_utils
from ui_components import NewRecordDialog, show_silent_info, show_silent_error, ask_silent_yesno

try:
    import tkinter as tk
    from tkinter import ttk
except Exception:
    sys.exit(1)

def _set_window_icon(win):
    try:
        if os.path.exists(config.ICON_FILE):
            win.iconbitmap(config.ICON_FILE)
    except Exception:
        pass

class LoanManagerApp(object):
    def __init__(self, root):
        self.root = root
        root.title("Gestor de Empréstimos Pessoal v{}".format(config.APP_VERSION))
        root.minsize(740, 580)
        _set_window_icon(root)

        msg = data_utils.init_db()
        if msg: show_silent_info(root, "Base de Dados", msg)

        self.style = ttk.Style()
        try: self.style.theme_use('clam')
        except Exception: pass

        self.daily_avail_var = tk.StringVar(value="...")
        self.monthly_avail_var = tk.StringVar(value="...")
        self.next_monthly_slot_var = tk.StringVar(value="...")
        self.status_bar_var = tk.StringVar(value="Pronto.")

        self.history_data_cache = None
        self.last_status_cache = None
        self.current_history_window = None
        self.current_history_tree = None
        self.current_history_search_var = None
        self._populate_history_tree = None
        self._search_job = None
        self.new_record_dialog_open = False
        
        self.expiries_last_sort_col = None
        self.expiries_last_sort_reverse = False
        self.history_win_last_sort_col = None
        self.history_win_last_sort_reverse = False

        self.create_menus()
        self.create_widgets()
        gui_utils.center_window(root)

        root.bind_all("<Control-h>", self.show_full_history)
        root.bind_all("<Control-n>", self._handle_new_record_dialog_event)
        root.bind_all("<F5>", lambda e: self.update_display())

        self.update_display()

    def _handle_new_record_dialog_event(self, event=None):
        self._handle_new_record_dialog()

    def create_menus(self):
        menu_bar = tk.Menu(self.root)
        self.root.config(menu=menu_bar)

        file_menu = tk.Menu(menu_bar, tearoff=0)
        menu_bar.add_cascade(label="Ficheiro", menu=file_menu)
        file_menu.add_command(label="Novo Registo", command=self._handle_new_record_dialog, accelerator="Ctrl+N")
        file_menu.add_command(label="Exportar BD para CSV", command=self.export_current_history)
        file_menu.add_separator()
        file_menu.add_command(label="Sair", command=self.root.quit)

        view_menu = tk.Menu(menu_bar, tearoff=0)
        menu_bar.add_cascade(label="Ver", menu=view_menu)
        view_menu.add_command(label="Histórico Completo", command=self.show_full_history, accelerator="Ctrl+H")
        view_menu.add_command(label="Estatísticas Avançadas", command=self.show_statistics_window)
        view_menu.add_separator()
        view_menu.add_command(label="Detalhe de Vagas", command=self.show_next_slots_dialog)

        help_menu = tk.Menu(menu_bar, tearoff=0)
        menu_bar.add_cascade(label="Ajuda", menu=help_menu)
        help_menu.add_command(label="Sobre", command=self.show_about)

    def create_widgets(self):
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.pack(expand=True, fill=tk.BOTH)

        self.create_limits_frame(main_frame).pack(fill=tk.X, pady=5)
        self.create_expiries_frame(main_frame).pack(fill=tk.BOTH, expand=True, pady=5)

        next_slot_frame = ttk.LabelFrame(main_frame, text="Disponibilidade Mensal Detalhada", padding="8")
        next_slot_frame.pack(fill=tk.X, pady=5)
        ttk.Label(next_slot_frame, textvariable=self.next_monthly_slot_var, justify=tk.LEFT, wraplength=680).pack(anchor=tk.W)

        ttk.Button(main_frame, text="Actualizar (F5)", command=self.update_display).pack(pady=8)

        status_frame = ttk.Frame(self.root)
        status_frame.pack(fill=tk.X, side=tk.BOTTOM)
        ttk.Label(status_frame, textvariable=self.status_bar_var, anchor=tk.W).pack(fill=tk.X, padx=4, pady=2)

    def create_limits_frame(self, parent):
        f = ttk.LabelFrame(parent, text="Limites Actuais", padding="10")
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Disponíveis Hoje (desde 00:00):").grid(row=0, column=0, sticky=tk.W, padx=5)
        ttk.Label(f, textvariable=self.daily_avail_var).grid(row=0, column=1, sticky=tk.E, padx=5)
        ttk.Label(f, text="Disponíveis Mês ({}d):".format(config.MONTHLY_WINDOW_DAYS)).grid(row=1, column=0, sticky=tk.W, padx=5)
        ttk.Label(f, textvariable=self.monthly_avail_var).grid(row=1, column=1, sticky=tk.E, padx=5)
        return f

    def create_expiries_frame(self, parent):
        f = ttk.LabelFrame(parent, text="Próximas Expirações de Empréstimos", padding="8")
        cols = ('title', 'expiry', 'left', 'type')
        self.expiries_tree = ttk.Treeview(f, columns=cols, show='headings', height=7)
        
        self.expiries_tree.heading('title', text='Título', command=lambda: self._sort_exp('title', False))
        self.expiries_tree.heading('expiry', text='Expira em', command=lambda: self._sort_exp('expiry', False))
        self.expiries_tree.heading('left', text='Dias', command=lambda: self._sort_exp('left', True))
        self.expiries_tree.heading('type', text='Tipo', command=lambda: self._sort_exp('type', False))

        self.expiries_tree.column('title', width=350)
        self.expiries_tree.column('expiry', width=130, anchor=tk.CENTER)
        self.expiries_tree.column('left', width=60, anchor=tk.CENTER)
        self.expiries_tree.column('type', width=90)

        sb = ttk.Scrollbar(f, orient=tk.VERTICAL, command=self.expiries_tree.yview)
        self.expiries_tree.configure(yscrollcommand=sb.set)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.expiries_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        return f

    def _sort_exp(self, col, is_num):
        gui_utils.sort_treeview_column(self.expiries_tree, col, is_num, 'expiries_last_sort_col', 'expiries_last_sort_reverse', self)

    def _handle_new_record_dialog(self):
        if self.new_record_dialog_open:
            self.root.bell()
            return
        
        self.new_record_dialog_open = True
        NewRecordDialog(self.root, self._finalize_new_record)

    def _finalize_new_record(self, res):
        self.new_record_dialog_open = False
        if not res: return
        ok, msg = data_utils.add_new_record_to_history(res['title'], res['loan_date'], res['type'])
        if ok:
            self.update_display()
            if self.current_history_window and self.current_history_window.winfo_exists():
                self.refresh_history_window()
            show_silent_info(self.root, "Sucesso", msg)
        else:
            show_silent_error(self.root, "Erro", msg)

    def update_display(self):
        try:
            metrics = data_utils.get_dashboard_metrics()
        except Exception as e:
            self.status_bar_var.set("Erro na leitura: {}".format(e))
            return
            
        self.last_status_cache = metrics
        
        self.daily_avail_var.set("{} / {}".format(metrics['daily_available'], config.MAX_DAILY_LOANS))
        self.monthly_avail_var.set("{} / {}".format(metrics['monthly_available'], config.MAX_MONTHLY_LOANS))
        
        self.expiries_tree.delete(*self.expiries_tree.get_children())
        for i, e in enumerate(metrics['expiries']):
            vals = (e['title'], e['expiry_date'].strftime(config.DATETIME_FORMAT), e['days_left'], e['type'])
            tag = 'even' if i%2==0 else 'odd'
            self.expiries_tree.insert('', tk.END, values=vals, tags=(tag,))
        self.expiries_tree.tag_configure('odd', background='white')
        self.expiries_tree.tag_configure('even', background='#F0F0F0')

        slots = data_utils.find_next_monthly_slot_dates(metrics['monthly_loans_list'])
        if metrics['monthly_available'] > 0:
            self.next_monthly_slot_var.set("Pode fazer empréstimos (limite mensal com vagas disponíveis).")
        else:
            if not slots: 
                self.next_monthly_slot_var.set("Limite atingido. Sem previsão de vaga próxima.")
            else:
                top_3 = slots[:3]
                lines = ["- " + s for s in top_3]
                txt = "Limite atingido. Próximas vagas a libertar:\n" + "\n".join(lines)
                self.next_monthly_slot_var.set(txt)
        
        self.status_bar_var.set("Última actualização: {}".format(datetime.datetime.now().strftime(config.DATETIME_FORMAT)))

    def show_next_slots_dialog(self):
        if not self.last_status_cache: 
            self.update_display()
        
        status = self.last_status_cache
        slots = data_utils.find_next_monthly_slot_dates(status['monthly_loans_list'])
        
        msg = "Estado Actual: {} vagas disponíveis.\n\n".format(status['monthly_available'])
        msg += "Empréstimos activos na janela de {} dias e data de libertação de vaga:\n".format(config.MONTHLY_WINDOW_DAYS)
        msg += "(Data/Hora em que deixam de contar para o limite)\n\n"
        if slots:
            for s in slots: 
                msg += "- {}\n".format(s)
        else:
            msg += "(Nenhum empréstimo activo a contar para o limite mensal)"
        
        show_silent_info(self.root, "Detalhe de Vagas", msg)

    def export_current_history(self):
        history_full = data_utils.read_history()
        out = os.path.join(
            config.EXPORT_DIR, 
            "bd_export_{}.csv".format(datetime.datetime.now().strftime("%Y%m%d_%H%M"))
        )
        ok, msg = data_utils.export_history_to_csv(history_full, out)
        if ok: 
            show_silent_info(self.root, "Exportar", msg)
        else: 
            show_silent_error(self.root, "Erro", msg)

    def show_full_history(self, event=None):
        if self.current_history_window and self.current_history_window.winfo_exists():
            self.current_history_window.lift()
            return
        
        win = tk.Toplevel(self.root)
        win.title("Histórico Completo")
        win.geometry("950x500")
        _set_window_icon(win)
        gui_utils.center_window(win)
        self.current_history_window = win

        frame = ttk.Frame(win, padding="10")
        frame.pack(fill=tk.BOTH, expand=True)
        
        sf = ttk.Frame(frame)
        sf.pack(fill=tk.X, pady=(0,10))
        ttk.Label(sf, text="Pesquisar Título:").pack(side=tk.LEFT)
        sv = tk.StringVar()
        se = ttk.Entry(sf, textvariable=sv)
        se.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=5)
        self.current_history_search_var = sv
        
        bf = ttk.Frame(frame)
        bf.pack(fill=tk.X, pady=(0,10))
        ttk.Button(bf, text="Editar", command=lambda: self._edit_sel(tree)).pack(side=tk.LEFT, padx=2)
        ttk.Button(bf, text="Eliminar", command=lambda: self._del_sel(tree)).pack(side=tk.LEFT, padx=2)
        ttk.Button(bf, text="Actualizar Lista", command=self.refresh_history_window).pack(side=tk.RIGHT)
        
        cols = ('id', 'date', 'title', 'type')
        tree = ttk.Treeview(frame, columns=cols, show='headings')
        tree.column('id', width=0, stretch=tk.NO)
        tree.heading('id', text='')
        tree.heading('date', text='Data/Hora', command=lambda: self._sort_hist(tree, 'date'))
        tree.heading('title', text='Título', command=lambda: self._sort_hist(tree, 'title'))
        tree.heading('type', text='Tipo', command=lambda: self._sort_hist(tree, 'type'))
        tree.column('date', width=140, anchor=tk.CENTER)
        tree.column('title', width=600)
        tree.column('type', width=100)
        
        sb = ttk.Scrollbar(frame, orient=tk.VERTICAL, command=tree.yview)
        tree.configure(yscrollcommand=sb.set)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.current_history_tree = tree
        
        def populate(data):
            tree.delete(*tree.get_children())
            for item in data:
                vals = (item['id'], item['date'].strftime(config.DATETIME_FORMAT), item['title'], item['type'])
                tree.insert('', tk.END, values=vals)
        
        self._populate_history_tree = populate
        
        def execute_search():
            t = sv.get().lower()
            d = self.history_data_cache if self.history_data_cache else []
            if t: 
                d = [x for x in d if t in x['title'].lower()]
            populate(d)

        def filter_h(*args):
            if self._search_job is not None:
                win.after_cancel(self._search_job)
            self._search_job = win.after(250, execute_search)
        
        sv.trace("w", filter_h)
        self.refresh_history_window()

    def _sort_hist(self, tree, col):
        gui_utils.sort_treeview_column(tree, col, False, 'history_win_last_sort_col', 'history_win_last_sort_reverse', self)

    def refresh_history_window(self):
        self.history_data_cache = data_utils.read_history()
        if self.current_history_tree and self._populate_history_tree:
            t = self.current_history_search_var.get().lower()
            d = self.history_data_cache
            if t: 
                d = [x for x in d if t in x['title'].lower()]
            self._populate_history_tree(d)

    def _del_sel(self, tree):
        sel = tree.selection()
        if not sel: 
            return
        
        if not ask_silent_yesno(self.current_history_window, "Eliminar", "Deseja eliminar os registos seleccionados?"): 
            return
        
        ids_to_del = []
        for s in sel:
            vals = tree.item(s, 'values')
            if vals: 
                ids_to_del.append(vals[0])
        
        errs = []
        for rid in ids_to_del:
            ok, msg = data_utils.delete_record(rid)
            if not ok: 
                errs.append(msg)
        
        if errs: 
            show_silent_error(self.current_history_window, "Aviso", "\n".join(errs))
        else: 
            show_silent_info(self.current_history_window, "Sucesso", "Registos eliminados com sucesso.")
        
        self.update_display()
        self.refresh_history_window()

    def _edit_sel(self, tree):
        sel = tree.selection()
        if not sel: 
            return
        
        vals = tree.item(sel[0], 'values')
        rid, d_str, tit, typ = vals[0], vals[1], vals[2], vals[3]
        dt = data_utils.parse_db_datetime(d_str)
        initial = {'title': tit, 'loan_date': dt if dt else None, 'type': typ}
        
        def cb(res):
            if not res: 
                return
            ok, msg = data_utils.edit_record(rid, res['title'], res['loan_date'], res['type'])
            if ok:
                self.update_display()
                self.refresh_history_window()
                show_silent_info(self.current_history_window, "Sucesso", msg)
            else:
                show_silent_error(self.current_history_window, "Erro", msg)
        
        NewRecordDialog(self.root, cb, initial)

    def show_statistics_window(self):
        win = tk.Toplevel(self.root)
        win.title("Estatísticas Avançadas (Painel de Bordo)")
        win.geometry("900x680")
        _set_window_icon(win)
        gui_utils.center_window(win)

        stats = data_utils.get_advanced_stats()

        nb = ttk.Notebook(win)
        nb.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        f1 = ttk.Frame(nb, padding=10)
        nb.add(f1, text="Geral e Cronologia")
        
        ttk.Label(f1, text="Total em Histórico: {}".format(stats['total_loans']), font=("Arial", 12, "bold")).pack(pady=5)
        bk = stats['types'].get('livro', 0)
        au = stats['types'].get('audiolivro', 0)
        ttk.Label(f1, text="Livros: {}  |  Audiolivros: {}".format(bk, au)).pack()

        lf_timeline = ttk.LabelFrame(f1, text="Evolução Anual", padding=10)
        lf_timeline.pack(fill=tk.BOTH, expand=True, pady=10)
        
        cv_time = tk.Canvas(lf_timeline, bg="white")
        cv_time.pack(fill=tk.BOTH, expand=True)
        
        years = sorted(stats['annual'].keys())
        vals = [stats['annual'][y] for y in years]
        if vals:
            max_v = max(vals) or 1
            w = 700
            h = 300
            bar_w = w / len(vals) * 0.6
            gap = w / len(vals) * 0.4
            x = 20
            for i, val in enumerate(vals):
                bar_h = (val / max_v) * (h - 40)
                y0 = h - 20
                y1 = y0 - bar_h
                cv_time.create_rectangle(x, y0, x+bar_w, y1, fill="#4a90e2")
                cv_time.create_text(x+bar_w/2, y0+10, text=years[i])
                cv_time.create_text(x+bar_w/2, y1-10, text=str(val))
                x += bar_w + gap

        f2 = ttk.Frame(nb, padding=10)
        nb.add(f2, text="Padrões de Actividade")
        
        top_frame = ttk.Frame(f2)
        top_frame.pack(fill=tk.X, pady=(0, 10))
        ttk.Label(top_frame, text="Ano:", font=("Arial", 10, "bold")).pack(side=tk.LEFT)
        
        avail_years = sorted(stats['annual'].keys(), reverse=True)
        if not avail_years: 
            avail_years = [str(datetime.datetime.now().year)]
        
        sel_year = tk.StringVar(value=avail_years[0])
        cb_year = ttk.Combobox(top_frame, textvariable=sel_year, values=avail_years, state="readonly", width=6)
        cb_year.pack(side=tk.LEFT, padx=5)

        ttk.Label(top_frame, text=" (Passe o rato (mouse) sobre os quadrados para ver a dica de contexto (tooltip))", 
                  font=("Arial", 8, "italic")).pack(side=tk.LEFT, padx=15)

        leg_frame = ttk.Frame(top_frame)
        leg_frame.pack(side=tk.RIGHT)
        colors = ["#ebedf0", "#c6e48b", "#7bc96f", "#239a3b", "#196127", "#1a3320"]
        ttk.Label(leg_frame, text="Menos", font=("Arial", 8)).pack(side=tk.LEFT, padx=2)
        for c in colors:
            tk.Label(leg_frame, bg=c, width=2).pack(side=tk.LEFT, padx=1)
        ttk.Label(leg_frame, text="Mais (5+)", font=("Arial", 8)).pack(side=tk.LEFT, padx=2)

        cv_cal = tk.Canvas(f2, bg="white")
        cv_cal.pack(fill=tk.BOTH, expand=True)

        def draw_calendar(*args):
            cv_cal.delete("all")
            target_year = int(sel_year.get())
            data = data_utils.get_activity_calendar_data(str(target_year))
            
            cell_s = 12
            gap = 2
            start_x = 30
            start_y = 40
            
            months = ["Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez"]
            for i, m in enumerate(months):
                cv_cal.create_text(start_x + (i * 4.5 * (cell_s+gap)), start_y - 15, text=m, font=("Arial", 8), anchor=tk.W)
            
            d_names = ["D", "S", "T", "Q", "Q", "S", "S"]
            for i, d in enumerate(d_names):
                if i % 2 == 1: 
                    cv_cal.create_text(start_x - 10, start_y + i*(cell_s+gap) + cell_s/2, text=d, font=("Arial", 7))

            d1 = datetime.date(target_year, 1, 1)
            d_end = datetime.date(target_year, 12, 31)
            
            curr = d1
            week_offset = 0
            
            while curr <= d_end:
                wd = (curr.weekday() + 1) % 7 
                
                if wd == 0 and curr != d1:
                    week_offset += 1
                
                count = data.get(curr, 0)
                c_idx = min(count, 5)
                fill = colors[c_idx]
                
                x1 = start_x + week_offset * (cell_s + gap)
                y1 = start_y + wd * (cell_s + gap)
                x2 = x1 + cell_s
                y2 = y1 + cell_s
                
                # Tag contendo informação embutida para a dica de contexto optimizada
                tag = "d_{}_{}".format(curr.strftime("%Y%m%d"), count)
                cv_cal.create_rectangle(x1, y1, x2, y2, fill=fill, outline="#d0d0d0", tags=(tag,))
                curr += datetime.timedelta(days=1)

            if not hasattr(self, 'cal_tooltip') or self.cal_tooltip is None:
                self.cal_tooltip = gui_utils.CanvasToolTip(cv_cal, delay=300)
            
            def get_tip_text(tags):
                for t in tags:
                    if t.startswith("d_"):
                        parts = t.split("_")
                        if len(parts) >= 3:
                            try:
                                dt = datetime.datetime.strptime(parts[1], "%Y%m%d").date()
                                count_val = int(parts[2])
                                return "{}: {} empréstimo(s)".format(dt.strftime("%d-%m-%Y"), count_val)
                            except ValueError:
                                pass
                return ""
            
            self.cal_tooltip.text_func = get_tip_text

        draw_calendar()
        cb_year.bind("<<ComboboxSelected>>", draw_calendar)

        f3 = ttk.Frame(nb, padding=10)
        nb.add(f3, text="Recordes")
        
        grp_rec = ttk.LabelFrame(f3, text="Recordes Pessoais", padding=10)
        grp_rec.pack(fill=tk.X, pady=10)
        ttk.Label(grp_rec, text="Maior Sequência (Dias consecutivos a ler): {}".format(stats['longest_streak'])).pack(anchor=tk.W)
        ttk.Label(grp_rec, text="Tempo Médio entre Empréstimos: {} dias".format(stats['avg_days_between'])).pack(anchor=tk.W)

    def show_about(self):
        msg = "Gestor de Empréstimos v{}\nPython {}.{}\nBD: SQLite\nSuporte integral para Windows XP.".format(
            config.APP_VERSION, sys.version_info.major, sys.version_info.minor)
        show_silent_info(self.root, "Sobre", msg)

if __name__ == "__main__":
    root = tk.Tk()
    app = LoanManagerApp(root)
    root.mainloop()
