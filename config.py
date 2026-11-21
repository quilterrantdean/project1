# config.py
# -*- coding: utf-8 -*-
# Configuração e constantes da aplicação

import os
import json

# --- Configuração padrão ---
DB_FILE = "biblioteca.db"
HISTORY_FILE_CSV = "historico.csv" 

ICON_FILE = "port.ico"
LOAN_DURATION_DAYS = 21
MONTHLY_WINDOW_DAYS = 30
MAX_DAILY_LOANS = 5
MAX_MONTHLY_LOANS = 15

# Formatos de data
DATETIME_FORMAT = '%Y-%m-%d %H:%M'
DATE_ONLY_FORMAT = '%Y-%m-%d'
TIME_ONLY_FORMAT = '%H:%M'

CSV_DELIMITER = ';'
CSV_HEADER = ['Title', 'LoanDate', 'Type'] 

APP_VERSION = "2.3.1"
APP_DATE = "2026-02-22"

EXPORT_DIR = "exports"
USER_SETTINGS_FILE = "user_settings.json"

try:
    if not os.path.exists(EXPORT_DIR):
        os.makedirs(EXPORT_DIR)
except Exception:
    pass

def load_user_settings():
    try:
        if os.path.exists(USER_SETTINGS_FILE):
            with open(USER_SETTINGS_FILE, 'r', encoding='utf-8') as f:
                data = json.load(f)
            global DB_FILE, LOAN_DURATION_DAYS, MONTHLY_WINDOW_DAYS, \
                   MAX_DAILY_LOANS, MAX_MONTHLY_LOANS, CSV_DELIMITER
            DB_FILE = data.get('DB_FILE', DB_FILE)
            LOAN_DURATION_DAYS = int(data.get('LOAN_DURATION_DAYS', LOAN_DURATION_DAYS))
            MONTHLY_WINDOW_DAYS = int(data.get('MONTHLY_WINDOW_DAYS', MONTHLY_WINDOW_DAYS))
            MAX_DAILY_LOANS = int(data.get('MAX_DAILY_LOANS', MAX_DAILY_LOANS))
            MAX_MONTHLY_LOANS = int(data.get('MAX_MONTHLY_LOANS', MAX_MONTHLY_LOANS))
            CSV_DELIMITER = data.get('CSV_DELIMITER', CSV_DELIMITER)
    except Exception:
        pass

def save_user_settings(settings_dict):
    try:
        with open(USER_SETTINGS_FILE, 'w', encoding='utf-8') as f:
            json.dump(settings_dict, f)
        return True, "Definições guardadas com sucesso."
    except Exception as e:
        return False, "Erro ao guardar definições: {}".format(e)

load_user_settings()