#!/bin/bash
echo alias recmp="/home/PKUOS/pintos/recmp.sh" >> /home/PKUOS/.bashrc
~/pintos/bash_completion_install.sh
bind -f  ~/.inputrc
source /root/.bashrc 
