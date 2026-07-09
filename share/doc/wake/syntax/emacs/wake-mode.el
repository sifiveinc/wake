;;; wake-mode.el --- mode for working with wake files

;; A major mode for editing wake files

;;; Code:

;;; Customization
(defgroup wake nil
  "Configuration for wake-mode."
  :prefix "wake-"
  :group 'wp)

(defcustom wake-tab-width 2
  "Width of a tab for wake."
  :group 'wake
  :type '(integer))

(defvar wake-keyword
  '("def" "target" "global" "export" "import" "from" "package" "type" "topic" "unary" "binary" "publish" "subscribe" "if" "then" "else" "here" "prim" "match" "require" "data" "tuple"))

(defvar wake-keyword-regex (regexp-opt wake-keyword 'words))
(defvar wake-identifier "\\([a-z_][A-Za-z0-9_]*\\)")
(defvar wake-op-regex "\\([.!&|,+~*/%<>$^-][.!&|,+~*/%<>$=^-]*\\|=[.!&|,+~*/%<>$=^-]+\\)")
(defvar wake-type (concat "\\([A-Z][A-Za-z0-9_]*" "\\|" wake-op-regex "\\)"))
(defvar wake-variable-start "\\(publish\\)")
(defvar wake-operator-arg-list (concat "\\(" wake-identifier "\s*\\)?" wake-op-regex "\s*" wake-identifier))

(defvar wake-font-lock-keywords
  `(;; publish
    (,(concat "publish\s+" wake-identifier "\s*=[\s\n\r]")
     (1 font-lock-warning-face))

    ;; operator defs
    (,(concat "\\(global\s+\\)?" "def\s+" wake-operator-arg-list)
     (4 font-lock-function-name-face))

    ;; normal defs
    (,(concat "\\(global\s+\\)?" "def\s+" wake-identifier "\\(\s+" wake-identifier "\\|\s*=[(A-Za-z0-9\s\r\n]\\|\s*\(\\)")
     (2 font-lock-function-name-face))

    ;; operator datas
    (,(concat "\\(global\s+\\)?" "data\s+" wake-operator-arg-list)
     (4 font-lock-type-face))

    ;; normal datas
    (,(concat "\\(global\s+\\)?" "data\s+" wake-identifier "\\(\s+" wake-identifier "\\|\s*=[\s\n\r]\\)")
     (2 font-lock-type-face))

    ;; tuple field
    (,(concat "\\(global\s+\\)?" "\\([A-Za-z0-9_]+\\)" "\s*:")
     (2 font-lock-function-name-face))

    ;; keywords
    (,wake-keyword-regex . font-lock-keyword-face)

    ;; operators in wake.prim
    (,wake-op-regex . font-lock-variable-name-face)

    ;; types
    (,(concat "\\<" wake-type ) . font-lock-type-face)

    ;; Literals
    ("\\<\\([0-9]+\\|[0-9]+\.[0-9]+\\|0[xX][0-9A-Fa-f_]+\\)\\>"
     (1 font-lock-constant-face))
    ))

(defun start-of-indent-block ()
  "indent if any of the following conditions are met:
1. previous line ends with 'if', 'then', or 'else'
2. previous line ends with '`something` ='
3. previous line ends with '('
4. previous line contains a 'match'
"
  (save-excursion
    (beginning-of-line-text)
    (backward-word)
    (beginning-of-line)

    ;; keep moving to previous line until we reach a non-comment
    (while (and (looking-at-p "\s*#") (not (bobp)))
      (backward-word)
      (beginning-of-line)
      )
    (beginning-of-line-text)

    (or
     (looking-at-p "\\(then\\|else\\|if\\)\s*\\(#.*\\)?$")
     (looking-at-p ".*[A-Za-z0-9_)(\s]=\s*\\(#.*\\)?$")
     (looking-at-p ".*\\<match\\>.*$")
     (looking-at-p ".*($")
     )
    )
  )

(defun previous-line-indent ()
  (save-excursion
    (beginning-of-line-text)
    (backward-word)
    (beginning-of-line)

    ;; keep moving to previous line until we reach a non-comment
    (while (and (looking-at-p "\s*#") (not (bobp)))
      (backward-word)
      (beginning-of-line)
      )

    (current-indentation)
    )
  )

;; Indentation
(defun wake-possible-indentations ()
  (let
      ((default (default-wake-possible-indentations))
       (current (current-indentation)))
    (if (and
         (/= current default)
         (/= (current-column) 0)
         (save-excursion
           (beginning-of-line)
           (not (looking-at-p "^\s*$")))
         )
        (number-sequence 0 current tab-width)
        (number-sequence 0 default tab-width)
      )
    )
  )

(defun default-wake-possible-indentations ()
  "Determine all possible indentations for a given line."
  (save-excursion
  (beginning-of-line)
  (let (indents)
    (cond ((or (bobp) (looking-at-p "\s*global"))
           (setq indents
                 0))

          ((start-of-indent-block)
           (setq indents
                 (+ (previous-line-indent) tab-width)))
          (t
           (setq indents
                 (previous-line-indent)))
          )
    indents
    )))

(defvar wake--indents)
(defun wake-cycle-indents ()
  "Indent the current wake line.
Uses 'wake-possible-indentations' to determine all possible
indentations for the given line and then cycles through these on
repeated key presses."
  (interactive)
  (if (eq last-command 'indent-for-tab-command)
      (setq wake--indents (append (last wake--indents)
                                    (butlast wake--indents)))
    (setq wake--indents (wake-possible-indentations)))
  (indent-line-to (car (last wake--indents))))


(defvar wake-table
  (let ((table (make-syntax-table text-mode-syntax-table)))
    (modify-syntax-entry ?\# "<" table)
    (modify-syntax-entry ?\n ">" table)
    (modify-syntax-entry ?\( "()" table)
    (modify-syntax-entry ?\) ")(" table)
    (modify-syntax-entry ?_ "w" table)
    (modify-syntax-entry ?\\ "\\" table)
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?\' "\"" table)
    (modify-syntax-entry ?` "\"`" table)
    table))

;;;###autoload
(define-derived-mode wake-mode text-mode "Wake"
  "Major mode for editing wake."
  (when wake-tab-width
    (setq tab-width wake-tab-width)) ;; Defined wake tab width

  ;; Set everything up
  (setq font-lock-defaults '(wake-font-lock-keywords))
  (setq-local indent-line-function 'wake-cycle-indents)
  (set-syntax-table wake-table)
  (setq-local comment-start "#")
  )

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.wake\\'" . wake-mode))

(provide 'wake-mode)
;;; wake-mode.el ends here
