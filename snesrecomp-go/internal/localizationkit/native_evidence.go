package localizationkit

// BuildNativeEvidence returns identified, single-ROM decompilation evidence.
// Keep the source identity and consumer proofs beside the catalogue, so a
// standalone diagnostic JSON file cannot be mistaken for a different release
// or for a playable language pack. No output files are written here.
func (d *Decoder) BuildNativeEvidence() (IRObject, error) {
	catalog, err := d.BuildNativeCatalog()
	if err != nil {
		return nil, err
	}
	reports, err := CheckNativeCoverage(catalog)
	if err != nil {
		return nil, err
	}
	return IRObject{
		"format": "actraiser-native-language-evidence", "version": 1,
		"source":          IRObject{"release_id": d.profile.ID, "locale": d.profile.Locale, "rom_sha256": d.profile.SHA256},
		"consumer_census": catalog.source, "catalog": catalog, "coverage": reports[0],
	}, nil
}
