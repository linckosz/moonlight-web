import os
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '.'
base = r'd:\Code\moonlight-web-deepseek'
full = os.path.join(base, path)
for root, dirs, files in os.walk(full):
    for f in files:
        print(os.path.join(root, f))
