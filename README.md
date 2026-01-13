# FocalTech FT9362 Fingerprint Driver

Disclaimer: I am not a real driver developer, nor an NN expert. This driver can
and will produce false positives and false negatives. Do not treat this as a
complete or secure solution for authentication.

Unfortunately, there is no open-source driver for the fingerprint sensor found
in the GPD Pocket 4. There was a build of libfprint with a driver for this
device (https://github.com/ftfpteams/focaltech-linux-fingerprint-driver/), but
it was removed due to a DMCA request. I tried to reverse-engineer the
aforementioned driver, but I was not able to properly reproduce either the image
processing or the validation. Most likely, the proprietary driver works much
better than mine, but it is no longer available.

During the development of this driver, I tried to implement multiple matching
algorithms and image-processing pipelines. However, due to the fact that this
sensor has a resolution of 80×76 pixels, it is impossible to extract fingerprint
ridges suitable for proper NBIS matching.

What remained was a CNN, and that is exactly what I used for this driver. The
dataset for this driver consists only of my own fingerprints. I captured 200
images of each finger using this device, and they were used for training. It
would be fair to say that the CNN might work only for me, since it is trained
solely on my own fingerprints. However, I doubt this is entirely the case, as at
this resolution you can barely distinguish one fingerprint from another.

If you want to create your own dataset, please open an issue or send me an
email, and I will try to provide the necessary tooling.

## What is it

This is a libfprint driver for the FocalTech FT9362 fingerprint sensor (USB
2808:0752) found in GPD Pocket 4. Essentially libfprint and fprintd are used as
glue to communicate with PAM, I am not using NBIS or BZ3 or anything that
libfprint provides. Reasons for that are stated above.

## Installation (NixOS)

Add to your `flake.nix`:

```nix
{
  inputs.gpd-fp.url = "github:shokerplz/ft9362-driver";

  outputs = { nixpkgs, gpd-fp, ... }: {
    nixosConfigurations.yourhost = nixpkgs.lib.nixosSystem {
      modules = [
        gpd-fp.nixosModules.default
        { services.fprintd-focaltech0752.enable = true; }
      ];
    };
  };
}
```

Then rebuild:

```bash
sudo nixos-rebuild switch --flake .
```

## Usage

```bash
fprintd-enroll    # Enroll fingerprint (15 samples)
fprintd-verify    # Verify
fprintd-list $USER
```

## Building from Source

```bash
nix build              # Build fprintd with driver
nix develop            # Enter dev shell
```

## Other distributions

If you need a package for your repo - please create an issue and I'll try to
figure something out. Or better just create a PR with building scripts.

## Technical Details

- Sensor: 80x76 pixel capacitive
- Matching: Siamese CNN with 5-stage verification
- Enrollment: 15 templates stored per finger

## License

LGPL-2.1-or-later
