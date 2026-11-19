# README picture sources

The pictures in `docs/readme` named banner, install-steps, disc-check,
rogue-arena-run, versus-room, direct-ip and ai-battles are made from these pages (launcher look,
Lato font from `recomp-ui/assets`). The belt patterns in `tiles/` come from
`games/master-of-arena/moa_hub_rank_tiles_data.h`, brightened
(`python3 tiles.py ../../../games/master-of-arena/moa_hub_rank_tiles_data.h`).

To remake one after editing its page (needs Google Chrome and Pillow):

    sh shot.sh banner.html ../banner.png
    sh shot.sh install.html ../install-steps.png
    sh shot.sh disc.html ../disc-check.png
    sh shot.sh rogue.html ../rogue-arena-run.png
    sh shot.sh room.html ../versus-room.png
    sh shot.sh directip.html ../direct-ip.png
    sh shot.sh ai.html ../ai-battles.png
