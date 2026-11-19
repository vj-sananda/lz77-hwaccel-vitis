#!/usr/bin/env python3
"""Create a derived include tree using BRAM on Kintex-7 (which has no URAM)."""
from pathlib import Path
import re
import shutil
root = Path(__file__).resolve().parent.parent
src = root / 'vendor/Vitis_Libraries/data_compression/L1/include/hw'
dst = root / 'build/vitis_include'
shutil.copytree(src, dst, dirs_exist_ok=True)
for path in dst.glob('*.hpp'):
    text = path.read_text()
    text = re.sub(r'(impl\s*=\s*)URAM\b', r'\1BRAM', text, flags=re.I)
    text = text.replace('#pragma HLS RESOURCE variable = dict core = XPM_MEMORY uram',
                        '#pragma HLS BIND_STORAGE variable = dict type = ram_t2p impl = bram')
    if path.name == 'zstd_compress.hpp':
        # Frame content sizes 256..65535 are encoded with a -256 bias.
        # Stored-block selection must use the original size, not the biased value.
        text = text.replace('        if (fcs > 255) {',
                            '        bool smallBlock = (fcs <= MIN_BLK_SIZE);\n        if (fcs > 255) {')
        text = text.replace('if (fcs < MIN_BLK_SIZE + 1)', 'if (smallBlock)')
    if path.name == 'zstd_compress_internal.hpp':
        # Upstream's single-symbol Huffman workaround only covers byte 0.
        # Apply its dummy-symbol mechanism to every single-symbol alphabet.
        old = '''            isRLE = (isRLE && cLit == 0);
            if (isRLE) {
                // printf("RLE literals\\n");
                literal_freq[3]++;
                outLitVal.data[0] = 3;'''
        new = '''            if (isRLE) {
                uint8_t dummy = (cLit == 3) ? 4 : 3;
                literal_freq[dummy]++;
                outLitVal.data[0] = dummy;'''
        if text.count(old) != 1:
            raise RuntimeError('Pinned L1 RLE patch context does not match')
        text = text.replace(old, new)
        # Normalization consumes LL, OF, ML, with ML expressed as length - 3.
        # The upstream stream sent LL, ML, OF; its OF lookup could therefore
        # read an uninitialized slot beyond the 32 valid offset frequencies.
        old = '''            outSeqFreqVal.data[0] = getMLCode<8>(outSeqVal.data[0].matlen);
            seqFreqStream << outSeqFreqVal;
            outSeqFreqVal.data[0] = bitsUsed31(outSeqVal.data[0].offset);
            seqFreqStream << outSeqFreqVal;'''
        new = '''            outSeqFreqVal.data[0] = seqCnt ? bitsUsed31(outSeqVal.data[0].offset) : 0;
            seqFreqStream << outSeqFreqVal;
            outSeqFreqVal.data[0] = seqCnt ? getMLCode<8>(outSeqVal.data[0].matlen - MIN_MATCH) : 0;
            seqFreqStream << outSeqFreqVal;'''
        if text.count(old) != 1:
            raise RuntimeError('Pinned L1 sequence patch context does not match')
        text = text.replace(old, new)
    if path.name == 'zstd_encoders.hpp':
        # Avoid undefined clz(0), unsigned underflow for small counts, and an
        # out-of-range rounding-table access whose value is only needed below 8.
        text = text.replace('auto maxBitsNeeded = bitsUsed31(symbolSize - 1) - 2;',
                            'int maxBitsNeeded = symbolSize > 1 ? int(bitsUsed31(symbolSize - 1)) - 2 : 0;\n'
                            '    if (maxBitsNeeded < 0) maxBitsNeeded = 0;')
        text = text.replace('rtbXvStepTable[proba]', 'rtbXvStepTable[proba < 8 ? proba : 0]')
    path.write_text(text)
print('Prepared Kintex-7 BRAM include tree:', dst)
