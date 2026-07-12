# Gera automaticamente um arquivo .hex a partir do .elf toda vez que o firmware
# for compilado. O MCUISP (e outros programadores tipo ISP) preferem .hex em vez
# de .bin porque o .hex ja carrega o endereco de memoria embutido.
#
# O arquivo final aparece em: .pio\build\bluepill_f103c8\firmware.hex

Import("env")

env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(
        '"$OBJCOPY" -O ihex "$BUILD_DIR/${PROGNAME}.elf" "$BUILD_DIR/${PROGNAME}.hex"',
        "Gerando $BUILD_DIR/${PROGNAME}.hex"
    )
)