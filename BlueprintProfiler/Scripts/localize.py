#!/usr/bin/env python3
"""
Script to convert LOCTEXT calls to BP_LOCTEXT for runtime localization support.
This script reads the translation files and replaces LOCTEXT calls with BP_LOCTEXT.
"""

import re
import os

def parse_po_file(filepath):
    """Parse .po file and return a dictionary of translations."""
    translations = {}
    current_msgid = None
    current_msgstr = None
    
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Parse msgid and msgstr pairs
    pattern = r'msgid "([^"]*)"\s+msgstr "([^"]*)"'
    matches = re.findall(pattern, content)
    
    for msgid, msgstr in matches:
        if msgid:  # Skip empty msgid
            translations[msgid] = msgstr
    
    return translations

def convert_loctext_to_bp(filepath, chinese_translations, english_translations):
    """Convert LOCTEXT calls to BP_LOCTEXT in a file."""
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Pattern to match LOCTEXT("Key", "DefaultText")
    pattern = r'LOCTEXT\("([^"]+)",\s*"([^"]+)"\)'
    
    def replace_match(match):
        key = match.group(1)
        default_text = match.group(2)
        
        # Get translations
        chinese = chinese_translations.get(key, default_text)
        english = english_translations.get(key, default_text)
        
        # Escape quotes in the text
        chinese = chinese.replace('"', '\\"')
        english = english.replace('"', '\\"')
        
        return f'BP_LOCTEXT("{key}", "{chinese}", "{english}")'
    
    new_content = re.sub(pattern, replace_match, content)
    
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(new_content)
    
    print(f"Converted {filepath}")

def main():
    # Paths
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    zh_po_path = os.path.join(base_dir, 'Content', 'Localization', 'BlueprintProfiler', 'zh-Hans', 'BlueprintProfiler.po')
    en_po_path = os.path.join(base_dir, 'Content', 'Localization', 'BlueprintProfiler', 'en', 'BlueprintProfiler.po')
    source_file = os.path.join(base_dir, 'Source', 'BlueprintProfiler', 'Private', 'UI', 'SBlueprintProfilerWidget.cpp')
    
    # Parse translation files
    print("Parsing Chinese translations...")
    chinese_translations = parse_po_file(zh_po_path)
    print(f"Found {len(chinese_translations)} Chinese translations")
    
    print("Parsing English translations...")
    english_translations = parse_po_file(en_po_path)
    print(f"Found {len(english_translations)} English translations")
    
    # Convert source file
    print("Converting source file...")
    convert_loctext_to_bp(source_file, chinese_translations, english_translations)
    
    print("Done!")

if __name__ == '__main__':
    main()
