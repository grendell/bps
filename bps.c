#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"

#define filelist(...) (FILE *[]) { __VA_ARGS__, NULL }

static const size_t LENGTH = 1024;
static uint8_t buffer[LENGTH];

static const size_t HEADER = 4;
static const size_t FOOTER = 12;

static const uint64_t SourceRead = 0ull;
static const uint64_t TargetRead = 1ull;
static const uint64_t SourceCopy = 2ull;
static const uint64_t TargetCopy = 3ull;

void finish(int status, FILE ** toClose, FILE * stream, const char * resultsFormat, ...) {
    if (toClose) {
        while (*toClose) {
            fclose(*toClose);
            ++toClose;
        }
    }

    va_list args;
    va_start(args, resultsFormat);
    vfprintf(stream, resultsFormat, args);
    va_end(args);

    exit(status);
}

const char * readNumber(FILE * input, uint64_t * num) {
    *num = 0;
    int mult = 1;

    while (1) {
        int x = fgetc(input);
        if (x < 0) {
            return "failed to read patch number\n";
        }

        *num += (x & 0x7f) * mult;

        if (x & 0x80) {
            break;
        }

        mult <<= 7;
        *num += mult;
    }

    return NULL;
}

const char * readSignedNumber(FILE * input, int64_t * num) {
    uint64_t x;
    const char * error = readNumber(input, &x);
    if (error) {
        return error;
    }

    *num = ((x & 1ull) ? -1ll : 1ll) * (x >> 1);
    return NULL;
}

const char * readMetadata(FILE * src, uint64_t length) {
    if (length) {
        printf("patch metadata:\n");
        size_t toRead = length;

        while (toRead > 0) {
            size_t s = toRead > LENGTH - 1 ? LENGTH - 1 : toRead;
            size_t r = fread(buffer, sizeof(uint8_t), s, src);

            if (r < 0) {
                return "failed to read patch metadata\n";
            }

            buffer[r] = '\0';
            printf("%s", buffer);

            toRead -= r;
        }

        printf("\n");
    } else {
        printf("no patch metadata available\n");
    }

    return NULL;
}

const char * copy(FILE * src, FILE * dst, uint64_t length) {
    size_t toRead = length;

    while (toRead > 0) {
        size_t s = toRead > LENGTH ? LENGTH : toRead;
        size_t r = fread(buffer, sizeof(uint8_t), s, src);

        if (r < 0) {
            return "failed to read patch data\n";
        }

        size_t toWrite = r;
        size_t offset = 0;

        while (toWrite > 0) {
            size_t w = fwrite(buffer + offset, sizeof(uint8_t), toWrite, dst);

            if (w < 0) {
                return "failed to write patch data\n";
            }

            toWrite -= w;
            offset += w;
        }

        toRead -= r;
    }

    return NULL;
}

const char * selfCopy(FILE * target, size_t readPos, uint64_t length) {
    size_t writePos = ftell(target);

    if (readPos + length > writePos) {
        for (uint64_t i = 0; i < length; ++i) {
            fseek(target, readPos++, SEEK_SET);
            int x = fgetc(target);
            if (x < 0) {
                return "failed to read patch data\n";
            }

            fseek(target, writePos++, SEEK_SET);
            x = fputc(x, target);
            if (x < 0) {
                return "failed to write patch data\n";
            }
        }
    } else {
        size_t toRead = length;

        while (toRead > 0) {
            size_t s = toRead > LENGTH ? LENGTH : toRead;
            fseek(target, readPos, SEEK_SET);
            size_t r = fread(buffer, sizeof(uint8_t), s, target);

            if (r < 0) {
                return "failed to read patch data\n";
            }

            size_t toWrite = r;
            size_t offset = 0;
            fseek(target, writePos, SEEK_SET);

            while (toWrite > 0) {
                size_t w = fwrite(buffer + offset, sizeof(uint8_t), toWrite, target);

                if (w < 0) {
                    return "failed to write patch data\n";
                }

                toWrite -= w;
                offset += w;
                writePos += w;
            }

            toRead -= r;
            readPos += r;
        }
    }

    return NULL;
}

int main(int argc, char ** argv) {
    if (argc != 4) {
        finish(1, NULL, stderr, "usage: %s <source.bin> <patch.bps> <target.bin>\n", argv[0]);
    }

    FILE * patch = fopen(argv[2], "rb");
    if (!patch) {
        finish(1, NULL, stderr, "failed to open %s\n", argv[2]);
    }

    size_t r = fread(buffer, sizeof(uint8_t), HEADER, patch);
    if (r != HEADER || strncmp((const char *) buffer, "BPS1", HEADER) != 0) {
        finish(1, filelist(patch), stderr, "invalid patch header\n");
    }

    fseek(patch, 0, SEEK_END);
    size_t patchSize = ftell(patch);

    uint32_t srcChecksum;
    uint32_t dstChecksum;
    fseek(patch, -12, SEEK_CUR);

    r = fread(&srcChecksum, sizeof(uint32_t), 1, patch);
    if (r != 1) {
        finish(1, filelist(patch), stderr, "failed to read patch checksum\n");
    }

    r = fread(&dstChecksum, sizeof(uint32_t), 1, patch);
    if (r != 1) {
        finish(1, filelist(patch), stderr, "failed to read patch checksum\n");
    }

    fseek(patch, HEADER, SEEK_SET);
    size_t targetReadOffset = 0;

    uint64_t srcSize;
    const char * error = readNumber(patch, &srcSize);
    if (error) {
        finish(1, filelist(patch), stderr, error);
    }

    FILE * source = fopen(argv[1], "rb");
    if (!source) {
        finish(1, filelist(patch), stderr, "failed to open %s\n", argv[1]);
    }

    fseek(source, 0, SEEK_END);
    size_t sourceSize = ftell(source);
    fseek(source, 0, SEEK_SET);

    if ((size_t) srcSize != sourceSize) {
        finish(1, filelist(source, patch), stderr, "unexpected file length: %s\n", argv[1]);
    }

    uint32_t sourceChecksum = calcCrc32(source);
    fseek(source, 0, SEEK_SET);

    if (srcChecksum != sourceChecksum) {
        finish(1, filelist(source, patch), stderr, "checksum mismatch: %s\n", argv[1]);
    }

    uint64_t dstSize;
    error = readNumber(patch, &dstSize);
    if (error) {
        finish(1, filelist(patch, source), stderr, error);
    }

    uint64_t metadataSize;
    error = readNumber(patch, &metadataSize);
    if (error) {
        finish(1, filelist(patch, source), stderr, error);
    }

    FILE * target = fopen(argv[3], "wb+");
    if (!target) {
        finish(1, filelist(source, patch), stderr, "failed to open %s\n", argv[3]);
    }

    FILE ** toClose = filelist(source, patch, target);

    error = readMetadata(patch, metadataSize);
    if (error) {
        finish(1, toClose, stderr, error);
    }

    while ((size_t) ftell(patch) < patchSize - FOOTER) {
        uint64_t data;
        error = readNumber(patch, &data);
        if (error) {
            finish(1, toClose, stderr, error);
        }

        uint64_t command = data & 3ull;
        uint64_t length = (data >> 2) + 1ull;

        switch (command) {
            case SourceRead: {
                size_t pos = ftell(source);
                size_t targetPos = ftell(target);
                fseek(source, targetPos, SEEK_SET);

                error = copy(source, target, length);
                if (error) {
                    finish(1, toClose, stderr, error);
                }

                fseek(source, pos, SEEK_SET);
                break;
            }
            case TargetRead: {
                error = copy(patch, target, length);
                if (error) {
                    finish(1, toClose, stderr, error);
                }
                break;
            }
            case SourceCopy: {
                int64_t offset;
                error = readSignedNumber(patch, &offset);
                if (error) {
                    finish(1, toClose, stderr, error);
                }

                fseek(source, offset, SEEK_CUR);

                error = copy(source, target, length);
                if (error) {
                    finish(1, toClose, stderr, error);
                }
                break;
            }
            case TargetCopy: {
                int64_t offset;
                error = readSignedNumber(patch, &offset);
                if (error) {
                    finish(1, toClose, stderr, error);
                }
                
                size_t readPos = targetReadOffset + offset;

                error = selfCopy(target, readPos, length);
                if (error) {
                    finish(1, toClose, stderr, error);
                }

                targetReadOffset = readPos + length;
                break;
            }
        }
    }

    size_t targetSize = ftell(target);
    if ((size_t) dstSize != targetSize) {
        finish(1, toClose, stderr, "unexpected file length: %s\n", argv[3]);
    }

    fseek(target, 0, SEEK_SET);
    uint32_t targetChecksum = calcCrc32(target);

    if (dstChecksum != targetChecksum) {
        finish(1, toClose, stderr, "checksum mismatch: %s\n", argv[3]);
    }

    finish(0, toClose, stdout, "patch applied successfully\n");
}