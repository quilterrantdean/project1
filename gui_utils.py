# gui_utils.py
# -*- coding: utf-8 -*-

try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:
    tk = None
    ttk = None

def center_window(window):
    if not tk: return
    window.update_idletasks()
    width = window.winfo_width()
    height = window.winfo_height()
    if width <= 1 or height <= 1:
        width = window.winfo_reqwidth()
        height = window.winfo_reqheight()
    x = (window.winfo_screenwidth() // 2) - (width // 2)
    y = (window.winfo_screenheight() // 2) - (height // 2)
    window.geometry('{}x{}+{}+{}'.format(width, height, x, y))

def sort_treeview_column(tv, col, is_numeric, last_sort_col_attr, last_sort_reverse_attr, component_instance):
    if not ttk: return
    items = [(tv.set(k, col), k) for k in tv.get_children('')]
    current_last_col = getattr(component_instance, last_sort_col_attr, None)
    current_last_reverse = getattr(component_instance, last_sort_reverse_attr, False)
    reverse = current_last_col == col and not current_last_reverse
    setattr(component_instance, last_sort_col_attr, col)
    setattr(component_instance, last_sort_reverse_attr, reverse)
    
    if is_numeric:
        try:
            items.sort(key=lambda t: int(t[0]) if t[0] != '' else -9999999, reverse=reverse)
        except ValueError:
            items.sort(key=lambda t: t[0], reverse=reverse)
    else:
        items.sort(key=lambda t: (t[0] or '').lower(), reverse=reverse)
        
    for index, (_, k) in enumerate(items):
        tv.move(k, '', index)

def handle_page_key(event, treeview, direction):
    if not ttk: return 'break'
    items_per_page = max(1, int(treeview.winfo_height() / 20))
    all_items = treeview.get_children('')
    if not all_items: return 'break'
    current_selection = treeview.selection()
    current_index = 0
    if current_selection:
        try:
            current_index = all_items.index(current_selection[0])
        except ValueError:
            pass
    new_index = max(0, min(len(all_items) - 1, current_index + (items_per_page * direction)))
    treeview.selection_set(all_items[new_index])
    treeview.see(all_items[new_index])
    return 'break'

def handle_arrow_key(event, treeview, direction):
    if not ttk: return 'break'
    all_items = treeview.get_children('')
    if not all_items: return 'break'
    current_selection = treeview.selection()
    if not current_selection:
        treeview.selection_set(all_items[0])
        treeview.see(all_items[0])
        return 'break'
    try:
        current_index = all_items.index(current_selection[0])
        new_index = max(0, min(len(all_items) - 1, current_index + direction))
        treeview.selection_set(all_items[new_index])
        treeview.see(all_items[new_index])
    except ValueError:
        pass
    return 'break'

class ToolTip(object):
    def __init__(self, widget, text, delay=500):
        self.widget = widget
        self.text = text
        self.delay = delay
        self._id_after = None
        self._tw = None
        widget.bind("<Enter>", self._on_enter)
        widget.bind("<Leave>", self._on_leave)
        widget.bind("<ButtonPress>", self._on_leave)

    def _on_enter(self, event=None):
        self._schedule()

    def _on_leave(self, event=None):
        self._unschedule()
        self._hide_tip()

    def _schedule(self):
        self._unschedule()
        self._id_after = self.widget.after(self.delay, self._show_tip)

    def _unschedule(self):
        if self._id_after:
            self.widget.after_cancel(self._id_after)
            self._id_after = None

    def _show_tip(self):
        if self._tw: return
        x = self.widget.winfo_rootx() + 20
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 1
        self._tw = tk.Toplevel(self.widget)
        self._tw.wm_overrideredirect(True)
        self._tw.geometry("+{}+{}".format(x, y))
        lbl = tk.Label(self._tw, text=self.text, justify=tk.LEFT,
                       background="#ffffe0", relief=tk.SOLID, borderwidth=1)
        lbl.pack(ipadx=4, ipady=2)

    def _hide_tip(self):
        if self._tw:
            self._tw.destroy()
            self._tw = None

class CanvasToolTip(object):
    """ Dica de contexto optimizada para uso em objectos dentro de um Canvas """
    def __init__(self, canvas, delay=300):
        self.canvas = canvas
        self.delay = delay
        self._id_after = None
        self._tw = None
        self._current_item = None
        self.text_func = None
        
        self.canvas.bind("<Motion>", self._on_motion)
        self.canvas.bind("<Leave>", self._on_leave)

    def _on_motion(self, event):
        x = self.canvas.canvasx(event.x)
        y = self.canvas.canvasy(event.y)
        items = self.canvas.find_overlapping(x, y, x, y)
        if not items:
            self._hide_tip()
            self._current_item = None
            return

        item = items[-1]
        if item != self._current_item:
            self._current_item = item
            self._unschedule()
            self._hide_tip()
            tags = self.canvas.gettags(self._current_item)
            if self.text_func:
                text = self.text_func(tags)
                if text:
                    self._id_after = self.canvas.after(
                        self.delay, 
                        lambda e=event, t=text: self._show_tip(e.x_root, e.y_root, t)
                    )
        else:
            pass 

    def _on_leave(self, event=None):
        self._current_item = None
        self._unschedule()
        self._hide_tip()

    def _unschedule(self):
        if self._id_after:
            self.canvas.after_cancel(self._id_after)
            self._id_after = None

    def _show_tip(self, x, y, text):
        if self._tw: return
        self._tw = tk.Toplevel(self.canvas)
        self._tw.wm_overrideredirect(True)
        self._tw.geometry("+{}+{}".format(x + 15, y + 15))
        lbl = tk.Label(self._tw, text=text, justify=tk.LEFT,
                       background="#ffffe0", relief=tk.SOLID, borderwidth=1,
                       font=("Arial", 9))
        lbl.pack(ipadx=4, ipady=2)

    def _hide_tip(self):
        if self._tw:
            self._tw.destroy()
            self._tw = None