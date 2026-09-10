package localization

// NativeDestinationCensus supplements known call-source discovery with the
// profiled raw/decoded destination and asset-font audits. Consumer completeness
// is not whole-game extraction/export completeness or graphical OCR coverage.
type NativeDestinationCensus struct {
	ConsumerComplete bool     `json:"whole_game_consumer_discovery_complete"`
	BG3Buffer        IRObject `json:"bg3_buffer_writes"`
	BG3Long          IRObject `json:"bg3_direct_long_writes"`
	BG3Decoded       IRObject `json:"bg3_decoded_address_range_audit"`
	VRAM             IRObject `json:"direct_vram_port_writes"`
	DMA              IRObject `json:"dma_launches"`
	Descriptor       IRObject `json:"generic_vram_descriptor"`
	Indirect         IRObject `json:"indirect_write_paths"`
	Font             IRObject `json:"dialog_font_asset"`
}

// Only called after fresh known-reader discovery from an identified ROM.
func (d *Decoder) destinationCensus(p destinationProfile, s sourceProfile, entries []assetEntry) (*NativeDestinationCensus, error) {
	base, long, err := d.bg3Destinations(p, s)
	if err != nil {
		return nil, err
	}
	decoded, err := d.decodedBG3Evidence(p)
	if err != nil {
		return nil, err
	}
	vram, err := d.directVRAMPaths(p)
	if err != nil {
		return nil, err
	}
	dma, err := d.dmaLaunches(p)
	if err != nil {
		return nil, err
	}
	descriptor, err := d.vramDescriptor(p)
	if err != nil {
		return nil, err
	}
	indirect, err := d.indirectDestinations(p)
	if err != nil {
		return nil, err
	}
	font, err := d.dialogFont(entries)
	if err != nil {
		return nil, err
	}
	return &NativeDestinationCensus{ConsumerComplete: true, BG3Buffer: base, BG3Long: long, BG3Decoded: decoded, VRAM: vram, DMA: dma, Descriptor: descriptor, Indirect: indirect, Font: font}, nil
}
