import os
import sys
from ikos import settings, analyzer
settings.ikos_analyzer = lambda: os.environ['IKOS_TEST_ANALYZER']
analyzer.main(sys.argv)
