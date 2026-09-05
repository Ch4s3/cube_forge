#!/bin/sh
# forge test, showing only errors, failures and the summary
forge test 2>&1 | awk '/^-- ERROR/{p=1} p{print} /^$/{if(p&&++b>1){p=0;b=0}} /Finished:|FAIL|error:/{print}' | grep -v "^$" | head -${1:-40}
