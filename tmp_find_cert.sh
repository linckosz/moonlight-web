#!/bin/bash
cd d:/Code/moonlight-web-deepseek
grep -rn "cert\|ssl\|SslServer\|QSsl\|loadCert\|certPath\|certificate\|acme" backend/src/server/ --include="*.cpp" --include="*.h" -l
