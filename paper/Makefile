.PHONY: all clean

all: main.pdf

main.pdf: main.tex references.bib $(wildcard data/*.dat)
	pdflatex -interaction=nonstopmode main.tex
	bibtex main
	pdflatex -interaction=nonstopmode main.tex
	pdflatex -interaction=nonstopmode main.tex

clean:
	rm -f main.aux main.bbl main.blg main.log main.out main.fls main.fdb_latexmk
