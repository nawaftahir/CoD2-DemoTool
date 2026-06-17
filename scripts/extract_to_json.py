import os
import re
import json

STOCK_DIR = "Refrences/stockrawfiles"
STR_DIRS = [
    f"{STOCK_DIR}/localized_english_iw09/localizedstrings",
    f"{STOCK_DIR}/localized_english_iw10/localizedstrings",
    f"{STOCK_DIR}/localized_english_iw11/localizedstrings"
]
WEAPON_DIR = f"{STOCK_DIR}/iw_13/weapons/mp"
OUT_FILE = "VerindraMod/dev/demo/CoD2-DemoTool/assets/localization.json"

output_data = {
    "weapons": {},
    "strings": {}
}

# 1. Parse .str files for Reference -> English text
for str_dir in STR_DIRS:
    if not os.path.exists(str_dir): continue
    for file in os.listdir(str_dir):
        if not file.endswith(".str"): continue
        with open(os.path.join(str_dir, file), 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
            # Match REFERENCE \s+ TAG \n LANG_ENGLISH \s+ "Text"
            matches = re.finditer(r'REFERENCE\s+([A-Za-z0-9_]+)\s*\n\s*LANG_ENGLISH\s+"([^"]+)"', content)
            prefix = file.replace('.str', '').upper() # e.g. MP, WEAPON, GAME, EXE
            for m in matches:
                tag = m.group(1).strip()
                text = m.group(2).strip()
                
                # Store the direct tag
                output_data["strings"][tag] = text
                # Also store the prefixed tag since the engine often prepends the filename 
                # (e.g. MP_JOINED_AXIS is formed by mp.str + JOINED_AXIS)
                output_data["strings"][f"{prefix}_{tag}"] = text

# 2. Parse weapon files for weapon_name_mp -> Reference
if os.path.exists(WEAPON_DIR):
    for w_file in os.listdir(WEAPON_DIR):
        if w_file.endswith("_mp"):
            with open(os.path.join(WEAPON_DIR, w_file), 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
                # Weapon files use \ to delimit key\value
                match = re.search(r'\\displayName\\([A-Za-z0-9_]+)', content)
                if match:
                    disp_name = match.group(1).replace('WEAPON_', '')
                    # Resolve the text from our parsed strings
                    resolved_text = output_data["strings"].get(disp_name) or output_data["strings"].get(f"WEAPON_{disp_name}")
                    if resolved_text:
                        output_data["weapons"][w_file] = resolved_text
                    else:
                        output_data["weapons"][w_file] = match.group(1) # fallback

# 3. Write to JSON
with open(OUT_FILE, 'w', encoding='utf-8') as f:
    json.dump(output_data, f, indent=4, sort_keys=True)

print(f"Successfully generated {OUT_FILE}")
print(f"Found {len(output_data['weapons'])} weapon tags and {len(output_data['strings'])} localized strings.")
