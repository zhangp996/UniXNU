# UniXNU A650 bring-up project (OnePlus 8 Pro / instantnoodlep)

Fresh rebuild following https://unixnu.com/zh/porting/steps:

-  - UniXNU core submodule (https://github.com/zhangp996/UniXNU)
-  - device repository for the OnePlus 8 Pro (SM8250, Adreno 650, iPadOS 17.7.11/21H461)
-  - our own A650 GPU kernel backend and support kits
-  - porting-step status and evidence

Build (from core/):

    export SM8250_A650_REFERENCE=/home/dev/unixnu/project-a650/../component/Adreno650
    export UNIXNU_DEVICE=/home/dev/unixnu/project-a650/../device
    python3 ./unixnu defconfig instantnoodlep_defconfig -D ../device --out ../out
    python3 ./unixnu build kernel -D ../device --out ../out
