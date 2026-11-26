"""Metadata for the GT4 source discs the importer accepts."""

RETAIL = '9ea0707b27466488fe14f68ebace3af31759efb13559d13584a3c714ef35a43c'


def supported(profiles):
    discs = profiles.get('discs')
    if discs:
        return discs
    # Older profile files and tests only carry the retail fingerprint.
    return {profiles['discTocSha256']: dict(name='Gran Turismo 4 USA v1.01', specdb='GT4_US2560',
                                            catalogueCars=722, core=True)}


def for_toc(profiles, toc):
    return supported(profiles).get(toc)


def names(profiles):
    return ', '.join(sorted({disc['name'] for disc in supported(profiles).values()}))
