/* Invented, ROM-free keyboard content shared by C contract/session fixtures. */
#define AR_TEST_KEYBOARD_PAGE(first) \
    "Test keyboard\n@line\n{master_name}\n@line\n--------\n@line\n" \
    first " B C D E F G H I J K L M\n@line\n" \
    "N O P Q R S T U V W X Y Z\n@line\n" \
    "a b c d e f g h i j k l m\n@line\n" \
    "n o p q r s t u v w x y z\n@line\n" \
    "0 1 2 3 4 5 6 7 8 9 . {icon.name_entry.backspace} {icon.name_entry.finish}\n"
