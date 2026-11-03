"""Build private enhancement routes from the unchanged, accepted fresh-card route.

These are exploratory qualification routes, not accepted performance references.
Trainer routes require a private settings.ini selecting the Trainer save profile.
"""
from pathlib import Path
import argparse

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    out=args.out.resolve()
    if out==ROOT or ROOT in out.parents:raise SystemExit('Output must be outside sources')
    out.mkdir(parents=True,exist_ok=False)
    cine=(ROOT/'testing/mvp/level_mvp_cine.tfs').read_text()
    end='          pass level running 300 pads after Enter Warp Gate'
    assert cine.count(end)==1
    graphics='''          trainer_probe
          setting renderer.hw_scale=1
          wait pads 30
          mark hardware_native
          setting renderer.hw_scale=4
          wait pads 30
          mark hardware_4x
          setting renderer.hw_fxaa=on
          wait pads 30
          mark hardware_4x_fxaa
          setting renderer.hw_scale=2
          wait pads 30
          mark hardware_2x_fxaa
          setting renderer.hw_fxaa=off
          setting renderer.hw_scale=4
          wait pads 30
          mark hardware_4x_restored
          pass hardware resolution and AA switching completed
'''
    (out/'graphics_scaling.tfs').write_text(cine.replace(end,graphics))
    hub='''          trainer_probe
          unlock minicons
          unlock levels
          wait pads 4
          trainer_probe
'''
    trainer=cine.replace('hubnav:',hub+'hubnav:',1)
    exercise='''          trainer_probe
          setting trainer.infinite_health=on
          setting trainer.unlimited_energy=on
          setting trainer.damage_multiplier=4
          setting trainer.movement_multiplier=1.5
          buttons r1
          stick 128 0 128 128
          wait pads 180 timeout 10000vs
          stick none
          wait pads 120 timeout 10000vs
          buttons none
          mark trainer_robot_enabled
          trainer_probe
          press triangle
          wait pads 45
          stick 128 0 128 128
          wait pads 60
          stick none
          mark trainer_vehicle_enabled
          trainer_probe
          press triangle
          wait pads 45
          setting trainer.infinite_health=off
          setting trainer.unlimited_energy=off
          setting trainer.damage_multiplier=1
          setting trainer.movement_multiplier=1
          wait pads 30
          mark trainer_disabled
          trainer_probe
          pass trainer toggles and transformations route completed
'''
    (out/'trainer_smoke.tfs').write_text(trainer.replace(end,exercise))
    reload=(ROOT/'testing/mvp/level2_save.tfs').read_text()
    reload=reload.split('missions:',1)[0]+'''          trainer_probe
          mark reloaded_hub
          pass saved profile reached hub; inspect trainer_probe for preserved unlocks
'''
    (out/'reload_hub.tfs').write_text(reload)
    print('Created exploratory routes in',out)

if __name__=='__main__':main()
