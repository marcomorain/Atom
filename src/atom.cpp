#include "atom.h"

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cassert>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>


// For a stack in parse_vector
#include <vector>

// For REPL
#include <readline/readline.h>

// fmod (in atom_modulo) brings in a dependancy on math.h
// todo: split / remove break?
#include <math.h>

#define DEBUG_LEXER (0)

#if (DEBUG_LEXER)
#define LEXER_TRACE(format, ...) printf(format, __VA_ARGS__)
#else
#define LEXER_TRACE(format, ...)
#endif


enum CellType
{
	TYPE_BOOLEAN,
	TYPE_CHARACTER,
	TYPE_NUMBER,
	TYPE_STRING,
	TYPE_PAIR,
	TYPE_VECTOR,
	TYPE_SYMBOL,
	TYPE_PROCEDURE,
	TYPE_INPUT_PORT,
	TYPE_OUTPUT_PORT
};

const static char* typenames [] = {
	"boolean",
	"character",
	"number",
	"string",
	"pair",
	"vector",
	"symbol",
	"procedure"
};

struct Environment;
struct Continuation;
struct Cell;

typedef Cell* (*atom_function) (Environment* env, Cell* params);

struct Cell
{
	struct Pair
	{
		Cell* car;
		Cell* cdr;
	};
	
	struct Vector
	{
		Cell** data;
		int    length;		
	};
	
	struct Procedure
	{
		// If function is null, then the procedure
		// was created in scheme, otherwise it is a built-in
		atom_function   function;
		Cell*    		formals;
		Cell*    		body;
		Environment*	env;
	};

	// todo: add a const string type? or a flag?
	// todo: add a length to string type

	union Data
	{
		bool         boolean;
		char         character;
		double       number;
		char*		 string;
		Pair         pair;
		const char*  symbol;
		Vector		 vector;
		Procedure    procedure;
		FILE*		 input_port;
        FILE*        output_port;
	};
	
	CellType type;
	Data	 data;
	Cell*    next;
	bool	 mark;
};

static Cell cell_true  = { TYPE_BOOLEAN, {true }, NULL, false };
static Cell cell_false = { TYPE_BOOLEAN, {false}, NULL, false };

enum Type
{
	TOKEN_IDENTIFIER,
	TOKEN_BOOLEAN,
	TOKEN_NUMBER,
	TOKEN_CHARACTER,
	TOKEN_STRING,
	TOKEN_LIST_START,
	TOKEN_LIST_END,
    TOKEN_VECTOR_START,
	TOKEN_QUOTE,
	TOKEN_BACKTICK,
	TOKEN_COMMA,
	TOKEN_COMMA_AT,
	TOKEN_DOT
};

static void print_rec(FILE* output, const Cell* cell, bool human, int is_car)
{
	if (!cell) return;

	switch(cell->type)
	{
	case TYPE_BOOLEAN:
		fprintf(output, "#%c", (cell->data.boolean ? 't' : 'f'));
		break;

	case TYPE_NUMBER:
		fprintf(output, "%lg", cell->data.number);
		break;

	case TYPE_CHARACTER:
		{
			char c = cell->data.character;
			
		    if (human)
		    {
                fputc(c, output);
		    }
		    else
		    {
		        switch(c)
			    {
				    case ' ':
    				fprintf(output, "#\\space");
    				break;
				
    				case '\n':
    				fprintf(output, "#\\newline");
    				break;
				
    				default:
    				fprintf(output, "#\\%c", c);
    				break;
    			}
			}
			break;
		}

	case TYPE_STRING:
		fprintf(output, human ? "%s" : "\"%s\"", cell->data.string);
		break;

	case TYPE_SYMBOL:
		fprintf(output, "%s", cell->data.symbol);
		break;

	case TYPE_PAIR:
		if (is_car) fprintf(output, "(");
		print_rec(output, cell->data.pair.car, human, 1);
		
		if (Cell* c = cell->data.pair.cdr)
		{
			fprintf(output, " ");
			if (c->type != TYPE_PAIR) fprintf(output, ". ");
			print_rec(output, c, human, 0);
		}

		if (is_car) fprintf(output, ")");
		break;
		
    case TYPE_INPUT_PORT:
        fprintf(output, "#<input port %p>", cell->data.input_port);
        break;
        
    case TYPE_OUTPUT_PORT:
        fprintf(output, "#<ouput port %p>", cell->data.input_port);
        break;
            
    case TYPE_VECTOR:
        fprintf(output, "#(");
        for (int i=0; i<cell->data.vector.length; i++)
        {
            if (i>0) fprintf(output, " ");
            print_rec(output, cell->data.vector.data[i], human, 0);
        }
        fprintf(output, ")");
        break;
	
	default:
		assert(false);
	}
}

static void print(FILE* output, const Cell* cell, bool human)
{
	print_rec(output, cell, human, 1);
	fprintf(output, "\n");
}


static bool power_of_two(int v)
{
	return v && !(v & (v - 1));
}

static bool is_integer(double d)
{
	return d == (int)d;
}


struct Environment
{
	struct Node
	{
		const char* symbol;
		Cell*       value;
		Node*		next;
	};

	Continuation* cont;
	Environment* parent;
	Node**		 data;
	unsigned	 mask;

	void init(Continuation* c, int size, Environment* parent_env)
	{
		assert(power_of_two(size));
		mask = size-1;
		const size_t num_bytes = size * sizeof(Node*);
		data = (Node**)malloc(num_bytes);
		memset(data, 0, num_bytes);
		parent = parent_env;
		cont = c;
	}
};


struct JumpBuffer
{
	jmp_buf     buffer;
	JumpBuffer* prev;
};

struct Continuation
{
	Environment*	env;
	Cell*			cells;
	JumpBuffer*		escape;
	int				allocated;
	FILE*			input;
    FILE*           output;
};

static void signal_error(Continuation* cont, const char* message, ...)
{
	va_list args;
	va_start(args, message);
	fprintf(stderr, "Error: ");
	vfprintf(stderr, message, args);
	fprintf(stderr, "\n");
	va_end(args);
	longjmp(cont->escape->buffer, 1);
}

static void type_check(Continuation* cont, int expected, int actual)
{
	if (actual != expected)
	{
		signal_error(cont, "%s expected, got %s", typenames[expected], typenames[actual]);
	}
}

static Cell* make_cell(Environment* env, int type)
{
	Cell* result = (Cell*)malloc(sizeof(Cell));
	memset(result, 0, sizeof(Cell));
	result->type = (CellType)type;

	// stick on the first item in the linked list
	result->next = env->cont->cells;
	env->cont->cells = result;
	env->cont->allocated++;

	return result;	
}

static Cell* make_io_port(Environment* env, int type, FILE* port)
{
	Cell* cell = make_cell(env, type);
	cell->data.input_port = port;
	return cell;
}


static Cell* make_input_port(Environment* env, FILE* port)
{
    return make_io_port(env, TYPE_INPUT_PORT, port);
}

static Cell* make_output_port(Environment* env, FILE* port)
{
    return make_io_port(env, TYPE_OUTPUT_PORT, port);
}

static void mark(Cell* cell);

static void mark_environment(Environment* env)
{
	for (unsigned i = 0; i <= env->mask; i++)
	{
		for (Environment::Node* node = env->data[i]; node; node = node->next)
		{
			mark(node->value);	
		}
	}
}

static void mark(Cell* cell)
{
	if (!cell || cell->mark) return;
	
	cell->mark = true;
	
	switch(cell->type)
	{
		case TYPE_BOOLEAN:
		case TYPE_CHARACTER:
		case TYPE_NUMBER:
		case TYPE_STRING:
		case TYPE_SYMBOL:
        case TYPE_INPUT_PORT:
        case TYPE_OUTPUT_PORT:
			break;
		
		case TYPE_PAIR:
			mark(cell->data.pair.car);
			mark(cell->data.pair.cdr);
			break;
			
		case TYPE_VECTOR:
            for(int i=0; i<cell->data.vector.length; i++)
            {
                mark(cell->data.vector.data[i]);
            }
			break;
		
		case TYPE_PROCEDURE:
		{
			Cell::Procedure& p = cell->data.procedure;
			if (!p.function)
			{
				mark(p.formals);
				mark(p.body);
				mark_environment(p.env);	
			}
			break;
		}
	}
}

static void collect_garbage(Continuation* cont)
{
    const int cells_before = cont->allocated;
	
	mark_environment(cont->env);
	
	Cell* remaining = NULL;
	Cell* next = NULL;
	
	for (Cell* cell = cont->cells; cell; cell = next)
	{
		next = cell->next;
		
		if (cell->mark)
		{
			cell->mark = false;
			cell->next = remaining;
			remaining = cell;
		}
		else
		{
		    switch(cell->type)
		    {
		        case TYPE_INPUT_PORT:
                if (cell->data.input_port != stdin)
                {
                    fclose(cell->data.input_port);
                }
                break;
                
                case TYPE_OUTPUT_PORT:
                if (cell->data.output_port != stdout)
                {
                    fclose(cell->data.output_port);
                }
                break;
                
                case TYPE_STRING:
                free(cell->data.string);
                break;
                
                case TYPE_VECTOR:
                free(cell->data.vector.data);
                break;
                
                default:
                break;
		    }
			cont->allocated--;
			free(cell);
		}
	}
	
	cont->cells = remaining;

	printf("GC: %d cells collected. %d remain allocated\n",
           cells_before - cont->allocated, cont->allocated);
	
}

static Cell* make_boolean(bool value)
{
	return value ? &cell_true : &cell_false;
}

static Cell* make_number(Environment* env, double value)
{
	Cell* number = make_cell(env, TYPE_NUMBER);
	number->data.number = value;
	return number;
}

static Cell* make_character(Environment* env, char c)
{
	Cell* character = make_cell(env, TYPE_CHARACTER);
	character->data.character = c;
	return character;	
}

static Cell* make_procedure(Environment* env, Cell* formals, Cell* body)
{
	type_check(env->cont, TYPE_PAIR, formals->type);
	type_check(env->cont, TYPE_PAIR, body->type);

	Cell* proc = make_cell(env, TYPE_PROCEDURE);
	proc->data.procedure.formals = formals;
	proc->data.procedure.body    = body;
	proc->data.procedure.env	 = env;
	return proc;	
}

static Cell* make_vector(Environment* env, int length, Cell* fill)
{
	Cell* vec = make_cell(env, TYPE_VECTOR);
	vec->data.vector.length = length;
	vec->data.vector.data   = (Cell**)malloc(length * sizeof(Cell*));
	for (int i=0; i<length; i++)
	{
		vec->data.vector.data[i] = fill;
	}
	return vec;
}

static Cell* car(const Cell* cell)
{
	assert(cell->type == TYPE_PAIR);
	return cell->data.pair.car;
}

static Cell* cdr(const Cell* cell)
{
	assert(cell->type == TYPE_PAIR);
	return cell->data.pair.cdr;
}

static void set_car(Cell* list, Cell* car)
{
	assert(list->type == TYPE_PAIR);
	list->data.pair.car = car;
}

static void set_cdr(Cell* list, Cell* cdr)
{
	assert(list->type == TYPE_PAIR);
	list->data.pair.cdr = cdr;
}

static Cell* cons(Environment* env, Cell* car, Cell* cdr)
{
	Cell* cell = make_cell(env, TYPE_PAIR);
	set_car(cell, car);
	set_cdr(cell, cdr);
	return cell;
}

static bool is_false(const Cell* cell)
{
	return	cell->type == TYPE_BOOLEAN &&
			cell->data.boolean == false;
}

struct Token
{
	void print(void) const
	{
		switch(type)
		{
		case TOKEN_NUMBER:
			LEXER_TRACE("Token TOKEN_NUMBER %lg\n", data.number);
			break;

		case TOKEN_IDENTIFIER:
			LEXER_TRACE("Token TOKEN_IDENTIFIER %s\n", data.identifier);
			break;

		case TOKEN_STRING:
			LEXER_TRACE("Token TOKEN_STRING \"%s\"\n", data.string);
			break;

#define PRINT_CASE(id) case id: LEXER_TRACE("Token %s\n", #id); break

		PRINT_CASE(TOKEN_BOOLEAN);
		PRINT_CASE(TOKEN_CHARACTER);
		PRINT_CASE(TOKEN_LIST_START);
		PRINT_CASE(TOKEN_LIST_END);
		PRINT_CASE(TOKEN_VECTOR_START);
		PRINT_CASE(TOKEN_QUOTE);
		PRINT_CASE(TOKEN_BACKTICK);
		PRINT_CASE(TOKEN_COMMA);
		PRINT_CASE(TOKEN_COMMA_AT);
		PRINT_CASE(TOKEN_DOT);
#undef PRINT_CASE
		}

	}

	union Data
	{
		double		number;
		bool		boolean;
		char*		string;
		const char*	identifier;
		char		character;
	};

	Type type;
	Data data;
};


struct TokenList
{
private:

	void add_basic(Type t)
	{
		tokens[next].type = t;
		tokens[next].print();
		next++;
	}

	char* buffer_copy_and_reset(void)
	{
		char* dup = (char*)malloc(buffer_position + 1);
		memcpy(dup, buffer_data, buffer_position);
		dup[buffer_position] = 0; // null terminate
		buffer_position = 0;
		return dup;
	}

public:

	void buffer_push(char c)
	{
		if (buffer_position == buffer_length)
		{
			buffer_length *= 2;
			buffer_data = (char*)realloc(buffer_data, buffer_length);
		}
		buffer_data[buffer_position] = c;
		buffer_position++;
	}

	char*  buffer_data;
	size_t buffer_length;
	size_t buffer_position;

	Environment* env;
	Token*	     tokens;
	int		     next;
	int		     length;
	
	void start_parse()
	{
		length = next;
		next = 0;
	}

	void add_backtick()
	{
		add_basic(TOKEN_BACKTICK);
	}

	void add_list_start()
	{
		add_basic(TOKEN_LIST_START);
	}

	void add_list_end()
	{
		add_basic(TOKEN_LIST_END);
	}
    
    void add_vector_start()
    {
        add_basic(TOKEN_VECTOR_START);
    }

	void add_quote()
	{
		add_basic(TOKEN_QUOTE);
	}

	void add_dot()
	{
		add_basic(TOKEN_DOT);
	}
    
    void add_comma_at()
    {
        add_basic(TOKEN_COMMA_AT);
    }
    
    void add_comma()
    {
        add_basic(TOKEN_COMMA);
    }

	void add_identifier(void)
	{
		tokens[next].type = TOKEN_IDENTIFIER;
		tokens[next].data.identifier = buffer_copy_and_reset();
		tokens[next].print();
		next++;
	}

	void add_string(void)
	{
		tokens[next].type = TOKEN_STRING;
		tokens[next].data.string = buffer_copy_and_reset();
		tokens[next].print();
		next++;
	}

	void add_number(double number)
	{
		tokens[next].type = TOKEN_NUMBER;
		tokens[next].data.number = number;
		tokens[next].print();
		next++;
	}
	
	void add_character(char value)
	{
		tokens[next].type = TOKEN_CHARACTER;
		tokens[next].data.character = value;
		tokens[next].print();
		next++;
	}

	void add_boolean(bool value)
	{
		tokens[next].type = TOKEN_BOOLEAN;
		tokens[next].data.boolean= value;
		tokens[next].print();
		next++;
	}

	void init(Environment* env, int size)
	{
		next   = 0;
		length = size;
		tokens = (Token*)malloc(size*sizeof(Token));

		buffer_position = 0;
		buffer_length	= 64;
		buffer_data = (char*)malloc(buffer_length);
		this->env = env;
	}

	void destroy()
	{
		free(tokens);
		free(buffer_data);
	}

	const Token* peek(void) const
	{
		if (next < length)
		{
			return tokens + next;
		}
		return NULL;
	}

	void skip(void)
	{
		next++;
	}
};


struct Input
{
	unsigned	line;
	unsigned	column;
	const char* data;
	TokenList*	tokens;
	Continuation* cont;

	void init(Continuation* c, const char* d)
	{
		line	= 1;
		column	= 1;
		data	= d;
		cont    = c;
		
	}
	
	char get(void)  const
	{
		return *data;
	};
	
	char next(void)
	{
		assert(*data);

		column++;

		if (*data == '\n')
		{
			column = 1;
			line++;
		}
		
		data++;
		return get();
	};
};

void syntax_error(Input& input, const char* message)
{
    signal_error(input.cont, "Syntax error line %d column %d: %s", input.line, input.column, message);
}

void skip_whitespace(Input& input)
{
	for(char c = input.get(); c; c = input.next())
	{
		switch(c)
		{
			case '\n':
			case ' ':
			case '\t':
			continue;
			
			case ';':
			for (char d = input.next(); d != '\n'; d = input.next())
			{
				if (!d) return;
			}
			break;
			
			default: return;
		}
	}
}

bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

bool is_special_initial(char c)
{
	switch (c)
	{
		case '!':
		case '$':
		case '%':
		case '&':
		case '*':
		case '/':
		case ':':
		case '<':
		case '=':
		case '>':
		case '?':
		case '^':
		case '_':
		case '~':
		return true;
	}

	return false;
}

static bool is_letter(char c)
{
	return !!isalpha(c);
}

static bool is_initial(char c)
{
	return is_letter(c) || is_special_initial(c);
}

bool is_delimeter(char c)
{
	switch (c)
	{
		case 0: // @todo: having null here is a bit of a hack - not in the spec.
		case ' ':
		case '\n':
		case '\t':
		case '"':
		case '(':
		case ')':
		case ';':
			return true;
	}
	return false;
}

bool is_special_subsequent(char c)
{
	switch(c)
	{
		case '+':
		case '-':
		case '.':
		case '@':
			return true;
	}

	return false;
}


static bool is_subsequent(char c)
{
	return is_initial(c) || is_digit(c) || is_special_subsequent(c);
}

static void read_character(Input& input)
{
	char c = input.get();
	switch(c)
	{
		case 's':
		if (input.next() == 'p'){
			if (input.next() != 'a') syntax_error(input, "space expected");
			if (input.next() != 'c') syntax_error(input, "space expected");
			if (input.next() != 'e') syntax_error(input, "space expected");
			if (!is_delimeter(input.next())) syntax_error(input, "space expected");
			input.tokens->add_character(' ');
			return;
		}
		else goto success;

		case 'n':
			if (input.next() == 'e'){
				if (input.next() != 'w') syntax_error(input, "newline expected");
				if (input.next() != 'l') syntax_error(input, "newline expected");
				if (input.next() != 'i') syntax_error(input, "newline expected");
				if (input.next() != 'n') syntax_error(input, "newline expected");
				if (input.next() != 'e') syntax_error(input, "newline expected");
				if (!is_delimeter(input.next())) syntax_error(input, "newline expected");
				input.tokens->add_character('\n'); // newline
				return;
			}
			else goto success;

		default: goto success;
	}

success:

	if (is_delimeter(input.next()))
	{
		input.tokens->add_character(c);
		return;
	}

	syntax_error(input, "delimeter expected");
}

// Convert an ascii digit, '0' to '9' into
// a double 0.0 to 9.0
static double char_to_double(char c)
{
	assert(c >= '0' && c <= '9');
	return c - '0';
}

void read_number(Input& input)
{
	char c = input.get();

	double accum = char_to_double(c);

	for (;;)
	{
		c = input.next();

		if (!is_digit(c))
		{
			input.tokens->add_number(accum);
			return;
		}
		
		accum *= 10;
		accum += char_to_double(c);
	}
}

void read_string(Input& input)
{
	assert(input.get() == '"');

	for (;;)
	{
		char c = input.next();

		if (c == '"'){
			input.next();
			input.tokens->add_string();
			return;
		}

		if (c == '\\'){
			c = input.next();
			if (c == '"' || c == '\\')
			{
				input.tokens->buffer_push(c);
				continue;
			}
			syntax_error(input, "malformed string");
		}

		if (isprint(c))
		{
			input.tokens->buffer_push(c);
			continue;
		}

		syntax_error(input, "unexpected character in string");
	}
}

bool is_peculiar_identifier(char c)
{
	// @todo: ... can be accepted here.
	return c == '+' || c == '-';
}

void read_identifier(Input& input)
{
	char c = input.get();
	if (is_initial(c))
	{
		input.tokens->buffer_push(c);

		for (;;)
		{
			c = input.next();
			if (is_delimeter(c)) break;
			if (!is_subsequent(c))
			{
				syntax_error(input, "malformed identifier");
			}
			input.tokens->buffer_push(c);
		}
	}
	else if (is_peculiar_identifier(c))
	{
		input.tokens->buffer_push(c);
		input.next();
	}
	else
	{
		syntax_error(input, "malformed identifier");
	}

	input.tokens->add_identifier();

}

void read_token(Input& input)
{
	skip_whitespace(input);
	
	char c = input.get();
	
	switch(c)
	{
		case '(':  input.next(); input.tokens->add_list_start(); break;
		case ')':  input.next(); input.tokens->add_list_end();   break;
		case '\'': input.next(); input.tokens->add_quote();      break;
		case '`':  input.next(); input.tokens->add_backtick();   break;
		case '.':  input.next(); input.tokens->add_dot();        break;
            
        case ',':
        {
            input.next();
            if(input.get() == '@')
            {
                input.next();
                input.tokens->add_comma_at();
            }
            else
            {
                input.tokens->add_comma();
            }
            
            break;
        }

		case '#':
		{
			c = input.next();
			switch(c)
			{
				// @todo: check for next character here (should be a delimiter)
				case 't':  input.next(); input.tokens->add_boolean(true);  break;
				case 'f':  input.next(); input.tokens->add_boolean(false); break;
				case '\\': input.next(); read_character(input);			   break;
                case '(':  input.next(); input.tokens->add_vector_start(); break;
                default:   syntax_error(input, "malformed identifier after #");
			}

			break;
		}

		case '"': read_string(input); break;

		case 0: break;
		
		default:
		{
			if (is_digit(c))
			{
				read_number(input);
			}
			else
			{
				read_identifier(input);
			}

			break;
		}
	}
}


Cell* parse_datum(TokenList& tokens);

Cell* parse_vector(TokenList& tokens)
{
    Environment* env = tokens.env;
    const Token* t = tokens.peek();
    if (!t) signal_error(env->cont, "unexpected end of input");
    
    if (t->type != TOKEN_VECTOR_START)
    {
        return NULL;
    }
    
    // skip the #(
    tokens.skip();
    
    int length = 0;
    
    std::vector<Cell*> stack;
    
    for (;;)
    {
        const Token* next = tokens.peek();
        if (!next) signal_error(env->cont, "unexpected end of input");
        if (next->type == TOKEN_LIST_END)
        {
            break;
        }
        stack.push_back(parse_datum(tokens));
        length++;
    }
                        
    Cell* vector = make_vector(env, length, NULL);
                        
    for (int i=0; i<length; i++)
    {
        vector->data.vector.data[i] = stack[i];
    }
    
	return vector;
}

Cell* parse_abreviation(TokenList& tokens)
{
	const Token* t = tokens.peek();
	
	Environment* env = tokens.env;

	if (!t) signal_error(env->cont, "unexpected end of input");
    
    const char* symbol = NULL;
    switch(t->type)
    {
        case TOKEN_QUOTE:
        symbol = "quote";
        break;
        
        case TOKEN_BACKTICK:
        symbol = "quasiquote";
        break;
        
        case TOKEN_COMMA:
        symbol = "unquote";
        break;
        
        case TOKEN_COMMA_AT:
        symbol = "unquote-splicing";
        break;
            
        // If the token is not one of the above abreviations, then we early out
        default:
        return NULL;
    }
     
    Cell* abreviation = make_cell(env, TYPE_SYMBOL);
    abreviation->data.symbol = symbol;
    tokens.skip();
    return cons(env, abreviation, cons(env, parse_datum(tokens), NULL));
}

Cell* parse_list(TokenList& tokens)
{
	// todo: pass this in
	Continuation* cont = tokens.env->cont;
	
	if (!tokens.peek()) return NULL;
	
	if (tokens.peek()->type != TOKEN_LIST_START)
	{
		return parse_abreviation(tokens);
	}

	// skip the start list token
	tokens.skip();
	
	Cell* cell = parse_datum(tokens);
	
	Cell* list = cons(tokens.env, cell, NULL);

	Cell* head = list;

	for (;;)
	{
		
		if (!tokens.peek())
		{
			signal_error(cont, "Unexpected end of input.");
		}
		
		if (tokens.peek()->type == TOKEN_DOT)
		{
			
			tokens.skip();
			Cell* cell = parse_datum(tokens);

			
			if (!cell)
			{
				signal_error(cont, "expecting a datum after a dot");
			}

			set_cdr(list, cell);

			if (tokens.peek()->type != TOKEN_LIST_END)
			{
				signal_error(cont, "expecting )");
			}

			tokens.skip();
			break;
		}
		else if (tokens.peek()->type == TOKEN_LIST_END)
		{
			tokens.skip();
			break; // success
		}
		
		Cell* car = parse_datum(tokens);

		if (!car)
		{
			signal_error(cont, "is this unexpected end of input? todo");
		}

		Cell* rest = cons(tokens.env, car, NULL);
		set_cdr(list, rest);
		list = rest;
	}

	// success, allocate a list
	return head;
}

Cell* parse_compound_datum(TokenList& tokens)
{
	if (Cell* cell = parse_list(tokens))
	{
		return cell;
	}

	return parse_vector(tokens);
}

Cell* parse_simple_datum(TokenList& tokens)
{
	const Token* t = tokens.peek();
	
	if (!t) return NULL;

	switch (t->type)
	{
		case TOKEN_BOOLEAN:
		{
			Cell* cell = make_cell(tokens.env, TYPE_BOOLEAN);
			cell->data.boolean = t->data.boolean;
			tokens.skip();
			return cell;
		}

		case TOKEN_CHARACTER:
		{
			
			Cell* cell = make_cell(tokens.env, TYPE_CHARACTER);
			cell->data.character = t->data.character;
			tokens.skip();
			return cell;
		}

		case TOKEN_NUMBER:
		{
			Cell* cell = make_number(tokens.env, t->data.number);
			tokens.skip();
			return cell;
		}

		case TOKEN_IDENTIFIER:
		{
			Cell* cell = make_cell(tokens.env, TYPE_SYMBOL);
			cell->data.symbol = t->data.identifier;
			tokens.skip();
			return cell;
		}
		
		case TOKEN_STRING:
		{
			Cell* cell = make_cell(tokens.env, TYPE_STRING);
			cell->data.string = t->data.string;
			tokens.skip();
			return cell;
		}

		default:
			return NULL;
	}
}

Cell* parse_datum(TokenList& tokens)
{
	if (Cell* cell = parse_simple_datum(tokens))
	{
		return cell;
	}
	
	return parse_compound_datum(tokens);
}


//-----------------------------------------------------------------------------
// MurmurHash2, by Austin Appleby

// Note - This code makes a few assumptions about how your machine behaves -

// 1. We can read a 4-byte value from any address without crashing
// 2. sizeof(int) == 4

// And it has a few limitations -

// 1. It will not work incrementally.
// 2. It will not produce the same results on little-endian and big-endian
//    machines.

static unsigned int MurmurHash2 ( const void * key, int len)
{
	const unsigned int seed = 0xdbc;

	assert(sizeof(int) == 4);

	// 'm' and 'r' are mixing constants generated offline.
	// They're not really 'magic', they just happen to work well.

	const unsigned int m = 0x5bd1e995;
	const int r = 24;

	// Initialize the hash to a 'random' value

	unsigned int h = seed ^ len;

	// Mix 4 bytes at a time into the hash

	const unsigned char * data = (const unsigned char *)key;

	while(len >= 4)
	{
		unsigned int k = *(unsigned int *)data;

		k *= m; 
		k ^= k >> r; 
		k *= m; 

		h *= m; 
		h ^= k;

		data += 4;
		len -= 4;
	}

	// Handle the last few bytes of the input array

	switch(len)
	{
	case 3: h ^= data[2] << 16;
	case 2: h ^= data[1] << 8;
	case 1: h ^= data[0];
		h *= m;
	};

	// Do a few final mixes of the hash to ensure the last few
	// bytes are well-incorporated.

	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
} 


Cell* environment_get(Environment* env, const Cell* symbol)
{
	assert(symbol->type == TYPE_SYMBOL);
	const char* str = symbol->data.symbol;
	unsigned hash = env->mask & MurmurHash2(str, (int)strlen(str));

	for (Environment::Node* node = env->data[hash]; node; node = node->next)
	{
		if (strcmp(str, node->symbol) == 0)
		{
			return node->value;
		}
	}
		
	if (env->parent)
	{
		return environment_get(env->parent, symbol);
	}
		
	signal_error(env->cont, "reference to undefined identifier: %s", str);
	return NULL;
	
}

void environment_define(Environment* env, const char* symbol, Cell* value)
{
	unsigned index = env->mask & MurmurHash2(symbol, (int)strlen(symbol));

	for (Environment::Node* node = env->data[index]; node; node = node->next)
	{
		if (strcmp(symbol, node->symbol) == 0)
		{
			node->value = value;
			return;
		}
	}
	
	Environment::Node* node = new Environment::Node;
	node->symbol		= symbol;
	node->value			= value;
	node->next			= env->data[index];
	env->data[index]	= node;
}

void environment_set(Environment* env, const char* symbol, Cell* value)
{
	unsigned hash = MurmurHash2(symbol, (int)strlen(symbol));
		
	do {
		
		unsigned index = hash & env->mask;

		for (Environment::Node* node = env->data[index]; node; node = node->next)
		{
			if (strcmp(symbol, node->symbol) == 0)
			{
				node->value = value;
				return;
			}
		}
		
		env = env->parent;
	
	} while (env);
		
	signal_error(env->cont, "No binding for %s in any scope.", symbol);
}

static Cell* eval(Environment* env, Cell* cell);

static Cell* type_q_helper(Environment* env, Cell* params, int type)
{
	Cell* obj = eval(env, car(params));
	return make_boolean(obj->type == type);
}


static Cell* nth_param_any_optional(Environment* env, Cell* params, int n)
{
	for (int i=1; i<n; i++)
	{
		if (!(params = cdr(params)))
		{
            return NULL;
		}
	}
	
	if (!params)
	{
        return NULL;
	}
	
	return eval(env, car(params));
}

// return the nth parameter to a function.
// n is indexed from 1 for the first parameter, 2 for the second.

static Cell* nth_param_any(Environment* env, Cell* params, int n)
{
    Cell* result = nth_param_any_optional(env, params, n);
    
    if (!result)
    {
        signal_error(env->cont, "Too few parameters passed (%d expected)", n);
    }
    
    return result;
}

static Cell* nth_param_optional(Environment* env, Cell* params, int n, int type)
{
   	Cell* result = nth_param_any_optional(env, params, n);
	// todo: this error message should include 'n'
	
	if (result)
	{
	    type_check(env->cont, type, result->type);    
	}

	return result; 
}

// The same as nth_param_any, with an added type check.
// If the type does not match, then an error is signaled.
static Cell* nth_param(Environment* env, Cell* params, int n, int type)
{
	Cell* result = nth_param_any(env, params, n);
	// todo: this error message should include 'n'
	type_check(env->cont, type, result->type);
	return result;
}

static int nth_param_integer(Environment* env, Cell* params, int n)
{
	Cell* param = nth_param(env, params, n, TYPE_NUMBER);
	if (!is_integer(param->data.number))
	{
		// todo: better error message
		signal_error(env->cont, "Not an integer");
	}
	return (int)param->data.number;
}


// Evaluate and return the second parameter, if one exists.
// Return null otherwise.
static Cell* optional_second_param(Environment* env, Cell* params)
{	
	Cell* rest = cdr(params);
	
	if (!rest)
	{
		return NULL;
	}
	
	Cell* result = eval(env, car(rest));
	return result;
}

// 4.1.2
// Literal Expressions

// (quote <datum>) evaluates to <datum>. <Datum> may be any external
// representation of a Scheme object (see section 3.3). This notation is
// used to include literal constants in Scheme code.
static Cell* atom_quote(Environment* env, Cell* params)
{
	return car(params);
}

// 4.1.5 Conditionals

// (if <test> <consequent> <alternate>)  syntax
// (if <test> <consequent>)              syntax
// Syntax: <Test>, <consequent>, and <alternate> may be arbitrary
// expressions.
// Semantics: An if expression is evaluated as follows: first, <test> is
// evaluated. If it yields a true value (see section 6.3.1), then
// <consequent> is evaluated and its value(s) is(are) returned. Otherwise
// <alternate> is evaluated and its value(s) is(are) returned.
// If <test> yields a false value and no <alternate> is specified, then
// the result of the expression is unspecified.
static Cell* atom_if(Environment* env, Cell* params)
{
	Cell* test = nth_param_any(env, params, 1);
	
	if (test->type == TYPE_BOOLEAN &&
		test->data.boolean == false)
	{
		Cell* alternate = cdr(cdr(params));
		if (alternate && car(alternate))
		{
			return eval(env, car(alternate));
		}
	
		// undefined, this is false though.
		return test;
	}
	
	// else eval consequent
	return eval(env, car(cdr(params)));
}

// 4.1.6. Assignments

// (set! <variable> <expression>)
// <Expression> is evaluated, and the resulting value is stored in the
// location to which <variable> is bound. <Variable> must be bound either
// in some region enclosing the set! expression or at top level. The result
// of the set! expression is unspecified.

static Cell* atom_set_b(Environment* env, Cell* params)
{
	Cell* variable   = car(params);
	type_check(env->cont, TYPE_SYMBOL, variable->type);
	Cell* expression = eval(env, car(cdr(params)));
	
	// @todo: seperate env->set and env->define
	environment_set(env, variable->data.symbol, expression);
	return expression;
}

// 4.2.1. Conditionals
// (cond <clause1> <clause2> ...) library syntax

// Syntax: Each <clause> should be of the form
// (<test> <expression1> ...)
// where <test> is any expression.
// Alternatively, a <clause> may be of the form
// (<test> => <expression>)
// The last <clause> may be an “else clause,” which has the form
// (else <expression1> <expression2> ...)

// Semantics: A cond expression is evaluated by evaluating the <test>
// expressions of successive <clause>s in order until one of them evaluates
// to a true value. When a <test> evaluates to a true value, then the
// remaining <expression>s in its <clause> are evaluated in order, and the
// result(s) of the last <expression> in the <clause> is(are) returned as
// the result(s) of the entire cond expression. If the selected <clause>
// contains only the <test> and no <expression>s, then the value of the
// <test> is returned as the result.

// If the selected <clause> uses the => alternate form, then the
// <expression> is evaluated. Its value must be a procedure that accepts
// one argument; this procedure is then called on the value of the <test>
// and the value(s) returned by this procedure is(are) returned by the cond
// expression. If all <test>s evaluate to false values, and there is no
// else clause, then the result of the conditional expression is
// unspecified; if there is an else clause, then its <expression>s are
// evaluated, and the value(s) of the last one is(are) returned.

static Cell* atom_cond(Environment* env, Cell* params)
{
	for(Cell* clause = params; clause; clause = cdr(clause))
	{
		Cell* test = car(clause);

		// @todo: make sure all symbols are stored in lowercase
		// @todo: assert that else is in the last place in the case
		// statement.
		Cell* t = car(test);
		if (t->type != TYPE_SYMBOL ||
			strcmp("else", t->data.string) != 0)
		{
			Cell* result = eval(env, t);	
			if (result->type == TYPE_BOOLEAN &&
				result->data.boolean == false)
			{
				continue;
			}
		}
		
		Cell* last_result = NULL;
		
		// @todo: assert there is at least one expression.
		for (Cell* expr = cdr(test); expr; expr = cdr(expr))
		{
			last_result = eval(env, car(expr));
		}
		
		return last_result;
	}
	
	// undefined.
	return make_boolean(false);
}

// (case <key> <clause1> <clause2> ...) library syntax
// Syntax: <Key> may be any expression. Each <clause> should have the form
//  ((<datum1> ...) <expression1> <expression2> ...),
// where each <datum> is an external representation of some object. All the
// <datum>s must be distinct. The last <clause> may be an “else clause,”
// which has the form
//  (else <expression1> <expression2> ...).
// Semantics:
// A case expression is evaluated as follows. <Key> is evaluated and its
// result is compared against each <datum>. If the result of evaluating <key> 
// is equivalent (in the sense of eqv?; see section 6.1) to a <datum>, then
// the expressions in the corresponding <clause> are evaluated from left to
// right and the result(s) of the last expression in
// the <clause> is(are) returned as the result(s) of the case expression. If
// the result of evaluating <key> is different from every <datum>, then if
// there is an else clause its expressions are evaluated and the result(s) of
// the last is(are) the result(s) of the case expression; otherwise the
// result of the case expression is unspecified.
static Cell* atom_case(Environment* env, Cell* params)
{
	//Cell* key = nth_param(env, params, 1, TYPE_NUMBER);
	// todo
	return NULL;
	
}

// (and <test1> ...)  library syntax
// The <test> expressions are evaluated from left to right, and the value of
// the first expression that evaluates to a false value (see section 6.3.1)
// is returned. Any remaining expressions are not evaluated. If all the
// expressions evaluate to true values, the value of the last expression is
// returned. If there are no expressions then #t is returned.
static Cell* atom_and(Environment* env, Cell* params)
{
	if (!car(params))
	{
		signal_error(env->cont, "syntax error. at least 1 test exptected in (and ...)");
	}

	Cell* last_result;
	for (Cell* cell = params; cell; cell = cdr(cell))
	{
		last_result = eval(env, car(cell));
		
		if (is_false(last_result))
		{
			return last_result;
		}
	}
	
	return last_result;
}

// (or	<test1> ...) library syntax
// The <test> expressions are evaluated from left to right, and the value of
// the first expression that evaluates to a true value (see section 6.3.1) is
// returned. Any remaining expressions are not evaluated. If all expressions
// evaluate to false values, the value of the last expression is returned. If
// there are no expressions then #f is returned.
static Cell* atom_or(Environment* env, Cell* params)
{
	if (!car(params))
	{
		signal_error(env->cont, "syntax error. at least 1 test exptected in (or ...)");
	}

	for (Cell* cell = params; cell; cell = cdr(cell))
	{
		Cell* test = eval(env, car(cell));
		
		if (is_false(test))
		{
			continue;
		}
		
		return test;
	}
	
	return make_boolean(false);
}


static Environment* create_environment(Continuation* cont, Environment* parent)
{
	Environment* env = (Environment*)malloc(sizeof(Environment));
	env->init(cont, 1, parent);
	return env;
}

// This function imeplements let and let*
// The only difference is the environment in which each init is evaluated in.
static Cell* let_helper(Environment* env, Cell* params, bool star)
{
	Cell* bindings = car(params);
	Cell* body     = cdr(params);
	
	if (!body)
	{
		signal_error(env->cont, "No expression in body");
	}
	
	Environment* child = create_environment(env->cont, env);
	
	Environment* target = star ? child : env;

	for (Cell* b = bindings; b; b = cdr(b))
	{
		Cell* pair = car(b);
		Cell* symbol = car(pair);
		type_check(env->cont, TYPE_SYMBOL, symbol->type);
		Cell* init   = eval(target, car(cdr(pair)));
		environment_define(child, symbol->data.symbol, init);
	}
	
	Cell* last = NULL;

	for (Cell* b = body; b; b = cdr(b))
	{
		Cell* expr = car(b);
		last = eval(child, expr);
	}
	
	return last;
}


// (let <bindings> <body>) library syntax
// Syntax: <Bindings> should have the form
// ((<variable1> <init1>) ...), where each <init> is an expression, and <body> should be a
// sequence of one or more expressions. It is an error for a <variable> to appear more than once in
// the list of variables being bound.
//
// Semantics: The <init>s are evaluated in the current environment (in some unspecified order), the
// <variable>s are bound to fresh locations holding the results, the <body> is evaluated in the
// extended environment, and the value(s) of the last expression of <body> is(are) returned. Each
// binding of a <variable> has <body> as its region.
static Cell* atom_let(Environment* env, Cell* params)
{
	return let_helper(env, params, false);
}

// (let* <bindings> <body>) Library syntax
// Syntax: <Bindings> should have the form
// ((<variable1> <init1>) ...), and <body> should be a sequence of one or more expressions.
//
// Semantics: Let* is similar to let, but the bindings are performed sequentially from left to right,
// and the region of a binding indicated by (<variable> <init>) is that part of the let* expression
// to the right of the binding. Thus the second binding is done in an environment in which the first
// binding is visible, and so on.
static Cell* atom_let_s(Environment* env, Cell* params)
{
	return let_helper(env, params, true);
}

static Cell* atom_define(Environment* env, Cell* params)
{
	Cell* first  = car(params); // no eval

	Cell* variable = 0;
	Cell* value    = 0;
	
	switch(first->type)
	{
		case TYPE_SYMBOL:
		{
			variable	= first;
			value		= eval(env, car(cdr(params)));
			break;
		}
		
		case TYPE_PAIR:
		{
			// todo: handle dotted syntax
			variable		= car(first);
			Cell* formals	= cdr(first);
			Cell* body		= cdr(params);
			value = make_procedure(env, formals, body);
			break;
		}
		
		default:
		// todo: make this a syntax error.
		signal_error(env->cont, "symbol or pair expected as parameter 1 to define");
	}
	
	assert(variable && value);
	type_check(env->cont, TYPE_SYMBOL, variable->type);
	environment_define(env, variable->data.symbol, value);
	// undefined result.
	return make_boolean(false);
}


static Cell* duplicate(Environment* env, Cell* list)
{
    if (list == NULL) return NULL;
    assert(list->type == TYPE_PAIR);
    return cons(env, car(list), cdr(list));
}

static Cell* append_destructive(Cell* a, Cell* b)
{
    if (!a) return b;
    
    Cell* current = a;
    
    for(;;)
    {
        if (cdr(current) == NULL)
        {
            set_cdr(current, b);
            return a;
        }
        current = cdr(current);
    }
    assert(false);
    return NULL;
}


static bool symbol_is(const Cell* symbol, const char* name)
{
    assert(symbol && symbol->type == TYPE_SYMBOL);
    return 0 == strcmp(name, symbol->data.symbol);
}

// TODO: Handle literal vectors in quasiquote
static Cell* quasiquote_helper(Environment* env, Cell* list)
{
    // break recursion
    if (!list) return NULL;
    
    // If the object is not a list, then there is nothing to do
    // TODO: vector literals
    if (list->type != TYPE_PAIR) return list;
    
    // At the end of the function we are going to
    // cons new_head onto recurse(rest)
    // The function modifies new_head
    Cell* head     = car(list);
    Cell* rest     = cdr(list);
    Cell* new_head = head;
    
    // TODO: make a proper empty list type, remove this line
    if (!head) return NULL;
    
    if (head->type == TYPE_PAIR)
    {
        Cell* operation = car(head);
        
        if (symbol_is(operation, "unquote"))
        {
            new_head = eval(env, car(cdr(head)));
        }
        else if (symbol_is(operation, "unquote-splicing"))
        {
            new_head = eval(env, car(cdr(head)));
            assert(new_head == NULL || new_head->type == TYPE_PAIR);
            return append_destructive(new_head, quasiquote_helper(env, rest));
        }
    }
    
    return cons(env, new_head, quasiquote_helper(env, rest));
}


// (quasiquote <qq template>) syntax
// `<qq template>             syntax
// “Backquote” or “quasiquote” expressions are useful for constructing a list or
// vector structure when most but not all of the desired structure is known in
// advance. If no commas appear within the ⟨qq template⟩, the result of evaluating
// `⟨qq template⟩ is equivalent to the result of evaluating ’⟨qq template⟩. If a
// comma appears within the ⟨qq template⟩, however, the expression following the
// comma is evaluated (“unquoted”) and its result is inserted into the structure
// instead of the comma and the expression. If a comma appears followed immediately
// by an atsign (@), then the following expression must evaluate to a list; the
// opening and closing parentheses of the list are then “stripped away” and the
// elements of the list are inserted in place of the comma at-sign expression
// sequence. A comma at-sign should only appear within a list or vector ⟨qq template⟩.
static Cell* atom_quasiquote(Environment* env, Cell* params)
{
    return quasiquote_helper(env, car(params));
}

static Cell* atom_error(Environment* env, Cell* params)
{
	Cell* message = nth_param_any(env, params, 1);
	
	const char* str = "Error";
	
	// todo: symantics here
	if (message && message->type == TYPE_STRING)
	{
		str = message->data.string;
	}
	signal_error(env->cont, "%s", str);
	return NULL;
}

static Cell* atom_lambda(Environment* env, Cell* params)
{
	return make_procedure(env, car(params), cdr(params));
}

// 4.2.3 Sequencing

// (begin <expression1> <expression> ...)	library syntax
// The <expression>s are evaluated sequentially from left to right, and
// the value(s) of the last <expression> is(are) returned. This expression
// type is used to sequence side ef- fects such as input and output.

static Cell* atom_begin(Environment* env, Cell* params)
{
	Cell* last = NULL;
	for (Cell* cell = params; cell; cell = cdr(cell))
	{
		// todo: tail recursion.
		last = eval(env, car(cell));
	}
	return last;
}

// 6.2.5 Numerical Operations


static Cell* plus_mul_helper(Environment* env,
							 Cell* params,
							 bool is_add,
							 double identity)
{
	double result = identity;
	
	for (Cell* z = params; z; z = cdr(z))
	{
		Cell* n = car(z);
		
		assert(n); // todo: trigger this assert and test
		
		Cell* value = eval(env, n);

		type_check(env->cont, TYPE_NUMBER, value->type);
			
		if (is_add)
		{
			result += value->data.number;
		}
		else
		{
			result *= value->data.number;
		}
	}
	return make_number(env, result);
}

// (+ z1 ...)
// Return the sum or product of the arguments.
static Cell* atom_plus(Environment* env, Cell* params)
{
	return plus_mul_helper(env, params, true, 0);
}

// (* z1 ...)
// Return the product of the arguments.
static Cell* atom_mul(Environment* env, Cell* params)
{
	return plus_mul_helper(env, params, false, 1);
}


static Cell* sub_div_helper(Environment* env, Cell* params, bool is_sub)
{
	Cell* z = nth_param(env, params, 1, TYPE_NUMBER);
	double initial = z->data.number;
	
	if (cdr(params))
	{
		for (Cell* cell = cdr(params); cell; cell = cdr(cell))
		{
			Cell* num = eval(env, car(cell));
			type_check(env->cont, TYPE_NUMBER, num->type);
			
			if (is_sub)
			{
				initial = initial - num->data.number;
			}
			else
			{
				initial = initial / num->data.number;
			}
		}
	}
	else
	{
		if (is_sub)
		{
			initial = -initial;
		}
		else
		{
			initial = 1/initial;
		}
	}
	
	return make_number(env, initial);
	
}

static Cell* atom_sub(Environment* env, Cell* params)
{
	return sub_div_helper(env, params, true);
}

static Cell* atom_div(Environment* env, Cell* params)
{
	return sub_div_helper(env, params, false);
}

static Cell* atom_modulo(Environment* env, Cell* params)
{
	Cell* a = nth_param(env, params, 1, TYPE_NUMBER);
	Cell* b = nth_param(env, params, 2, TYPE_NUMBER);
	return make_number(env, fmod(a->data.number, b->data.number));
}

// These numerical predicates provide tests for the exactness of a quantity.
// For any Scheme number, precisely one of these predicates is true.
static Cell* atom_exact_q(Environment* env, Cell* params)
{
	nth_param(env, params, 1, TYPE_NUMBER);
	return make_boolean(false);
}

static Cell* atom_inexact_q(Environment* env, Cell* params)
{
	nth_param(env, params, 1, TYPE_NUMBER);
	return make_boolean(true);
}

static bool eq_helper(const Cell* obj1, const Cell* obj2, bool recurse_strings, bool recurse_compound);

static bool pair_equal(const Cell* obj1, const Cell* obj2, bool recursive)
{	
	if (obj1 == obj2)   return true;
	if (!obj1 || !obj2) return false; // TODO: Test for nulls / empty lists
	if (!recursive)     return false;
	
	if (!eq_helper(car(obj1), car(obj2), true, true)) return false;
	
	return pair_equal(cdr(obj1), cdr(obj2), true);
}

static bool vector_equal(const Cell* obj1, const Cell* obj2, bool recursive)
{
	assert(obj1->type == TYPE_VECTOR);
	assert(obj2->type == TYPE_VECTOR);
	
	if (obj1 == obj2) return true;
	if (!recursive)   return false;
	
	const int length = obj1->data.vector.length;
	
	// if different lengths, return false
	if (obj2->data.vector.length != length) return false;
			
	Cell* const* const a = obj1->data.vector.data;
	Cell* const* const b = obj2->data.vector.data;
			
	for (int i=0; i<length; i++)
	{
		if (!eq_helper(a[i], b[i], true, true))
		{
			return false;
		}
	}
	return true;
}

static bool eq_helper(const Cell* obj1, const Cell* obj2, bool recurse_strings, bool recurse_compound)
{
	const int type = obj1->type;

	if (type != obj2->type)
	{
		return false;
	}
	
	switch(type)
	{
		case TYPE_BOOLEAN:
		return obj1->data.boolean == obj2->data.boolean;

		case TYPE_CHARACTER:
		return obj1->data.character == obj2->data.character;

		case TYPE_SYMBOL:
		// @todo: intern symbols, use pointer equality
		return 0 == strcmp(obj1->data.symbol, obj2->data.symbol);

		case TYPE_NUMBER:
		return obj1->data.number == obj2->data.number;
			
		case TYPE_PAIR:
		return pair_equal(obj1, obj2, recurse_compound);

		case TYPE_VECTOR:
		return vector_equal(obj1, obj2, recurse_compound);

		case TYPE_STRING:
		return (obj1 == obj2) ||
				(recurse_strings && (0 == strcmp(obj1->data.string, obj2->data.string)));

		default:
		// unhandled case
		assert(0);
	}

	return false;
}

// (eqv? obj1 obj2) procedure
// The eqv? procedure defines a useful equivalence relation on objects.
// Briefly, it returns #t if obj1 and obj2 should normally be regarded as the same object.
static Cell* atom_eqv_q(Environment* env, Cell* params)
{
	Cell* obj1 = nth_param_any(env, params, 1);
	Cell* obj2 = nth_param_any(env, params, 2);
	return make_boolean(eq_helper(obj1, obj2, true, false));
}

// (eq? obj1 obj2)	procedure
// Eq? is similar to eqv? except that in some cases it is capable of discerning
// distinctions finer than those detectable by eqv?.
// Eq? and eqv? are guaranteed to have the same behavior on symbols, booleans,
// the empty list, pairs, procedures, and non-empty strings and vectors.
// Eq?’s behavior on numbers and characters is implementation-dependent, but it
// will always return either true or false, and will return true only when eqv?
// would also return true. Eq? may also behave differently from eqv? on empty
// vectors and empty strings.
static Cell* atom_eq_q(Environment* env, Cell* params)
{
	Cell* obj1 = nth_param_any(env, params, 1);
	Cell* obj2 = nth_param_any(env, params, 2);
	return make_boolean(eq_helper(obj1, obj2, false, false));
}

// (equal? obj1 obj2)	library procedure
// Equal? recursively compares the contents of pairs, vectors, and strings,
// applying eqv? on other objects such as numbers and symbols. A rule of thumb is
// that objects are generally equal? if they print the same. Equal? may fail to
// terminate if its arguments are circular data structures.
static Cell* atom_equal_q(Environment* env, Cell* params)
{
	Cell* obj1 = nth_param_any(env, params, 1);
	Cell* obj2 = nth_param_any(env, params, 2);
	return make_boolean(eq_helper(obj1, obj2, true, true));
}

static Cell* atom_number_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_NUMBER);
}

static Cell* atom_integer_q(Environment* env, Cell* params)
{
	Cell* obj = nth_param_any(env, params, 1);
	
	bool integer =	obj->type == TYPE_NUMBER &&
					is_integer(obj->data.number);
	
	return make_boolean(integer);
}

template <typename Compare>
static Cell* comparison_helper(Environment* env, Cell* params)
{
	int n = 2;

	Cell* a = nth_param(env, params, 1, TYPE_NUMBER);
	
	for (;;)
	{
		params = cdr(params);
		Cell* b = nth_param(env, params, 1, TYPE_NUMBER);
	
		double x = a->data.number;
		double y = b->data.number;
		if (!Compare::compare(x, y))
		{
			return make_boolean(false);
		}
	
		a = b;
		n++;
	
		if (!cdr(params))
		{
			break;
		}
	}

	return make_boolean(true);
};

struct Equal		{ static bool compare(double a, double b) { return a == b; } };
struct Less			{ static bool compare(double a, double b) { return a <  b; } };
struct Greater		{ static bool compare(double a, double b) { return a >  b; } };
struct LessEq		{ static bool compare(double a, double b) { return a <= b; } };
struct GreaterEq	{ static bool compare(double a, double b) { return a >= b; } };

static Cell* atom_comapre_equal(Environment* env, Cell* params)
{
	return comparison_helper<Equal>(env, params);
}

static Cell* atom_compare_less(Environment* env, Cell* params)
{
	return comparison_helper<Less>(env, params);
}

static Cell* atom_compare_greater(Environment* env, Cell* params)
{
	return comparison_helper<Greater>(env, params);
}

static Cell* atom_compare_less_equal(Environment* env, Cell* params)
{
	return comparison_helper<LessEq>(env, params);
}

static Cell* atom_compare_greater_equal(Environment* env, Cell* params)
{
	return comparison_helper<GreaterEq>(env, params);
}

// (zero? z)
// (positive? x)
// (negative? x)
// (odd? n)
// (even? n)
// These numerical predicates test a number for a particular property, returning
// #t or #f.
static Cell* atom_zero_q(Environment* env, Cell* params)
{
	double result = nth_param(env, params, 1, TYPE_NUMBER)->data.number;
    return make_boolean(result == 0.0);
}

static Cell* atom_positive_q(Environment* env, Cell* params)
{
	double result = nth_param(env, params, 1, TYPE_NUMBER)->data.number;
    return make_boolean(result > 0.0);
}

static Cell* atom_negative_q(Environment* env, Cell* params)
{
	double result = nth_param(env, params, 1, TYPE_NUMBER)->data.number;
    return make_boolean(result < 0.0);
}

static Cell* atom_odd_q(Environment* env, Cell* params)
{
	int result = nth_param_integer(env, params, 1);
    return make_boolean(result & 1);
}

static Cell* atom_even_q(Environment* env, Cell* params)
{
	int result = nth_param_integer(env, params, 1);
    return make_boolean(0 == (result & 1));
}

// (max x1 x2 ...) library procedure
// (min x1 x2 ...) library procedure
// These procedures return the maximum or minimum of their arguments.

static Cell* min_max_helper(Environment* env, Cell* params, bool is_min)
{
	double result = nth_param(env, params, 1, TYPE_NUMBER)->data.number;
	
	for (Cell* x = cdr(params); x; x = cdr(x))
	{
		Cell* n = eval(env, car(x));
		type_check(env->cont, TYPE_NUMBER, n->type);
		
		if (is_min)
		{
			result = std::min(result, n->data.number);
		}
		else
		{
			result = std::max(result, n->data.number);
		}
	}
	return make_number(env, result);
}

static Cell* atom_min(Environment* env, Cell* params)
{
	return min_max_helper(env, params, true);
}

static Cell* atom_max(Environment* env, Cell* params)
{
	return min_max_helper(env, params, false);
}
// 6.3

// 6.3.1: Booleans

static Cell* atom_boolean_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_BOOLEAN);	
}

static Cell* atom_not(Environment* env, Cell* params)
{
	Cell* obj = eval(env, car(params));
	bool is_truthy = obj->type != TYPE_BOOLEAN || obj->data.boolean;
	return make_boolean(!is_truthy);
}

// 6.3.2 Pairs and lists


static Cell* atom_pair_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_PAIR);	
}

static Cell* atom_cons(Environment* env, Cell* params)
{
	Cell* first  = nth_param_any(env, params, 1);
	Cell* second = nth_param_any(env, params, 2);
	return cons(env, first, second);
}

static Cell* atom_car(Environment* env, Cell* params)
{
	Cell* list = nth_param(env, params, 1, TYPE_PAIR);
	return car(list);
}

static Cell* atom_cdr(Environment* env, Cell* params)
{
	Cell* list = nth_param(env, params, 1, TYPE_PAIR);
	return cdr(list);
}

static Cell* set_car_cdr_helper(Environment* env, Cell* params, int is_car)
{
	// @todo: make an error here for constant lists	
	Cell* pair = nth_param(env, params, 1, TYPE_PAIR);
	Cell* obj  = eval(env, car(cdr(params)));
	
	if (is_car)
	{
		pair->data.pair.car = obj;
	}
	else
	{
		pair->data.pair.cdr = obj;	
	}
	
	// return value here is unspecified
	return pair;	
}

static Cell* atom_set_car_b(Environment* env, Cell* params)
{
	return set_car_cdr_helper(env, params, 1);
}

static Cell* atom_set_cdr_b(Environment* env, Cell* params)
{
	return set_car_cdr_helper(env, params, 0);
}

// Returns #t if obj is the empty list, otherwise returns #f.
static Cell* atom_null_q(Environment* env, Cell* params)
{
	Cell* obj = nth_param_any(env, params, 1);
	return make_boolean(obj->type == TYPE_PAIR &&
						obj->data.pair.car == NULL &&
						obj->data.pair.cdr == NULL);
}

// (list? obj)
// Returns #t if obj is a list, otherwise returns #f. By definition, all
// lists have finite length and are terminated by the empty list.
static Cell* atom_list_q(Environment* env, Cell* params)
{
	Cell* obj = nth_param_any(env, params, 1);
	
	if (obj->type == TYPE_PAIR)
	{
		if (Cell* rest = obj->data.pair.cdr)
		{
			// @todo: should this recurse O(N)
			// to see it list terminates?
			return make_boolean(rest->type == TYPE_PAIR);
		}
		
		return make_boolean(true);
	}
	
	return make_boolean(false);
}

// (list obj ...)
// Returns a newly allocated list of its arguments.
static Cell* atom_list(Environment* env, Cell* params)
{
	// @todo: use an empty list type here.
	Cell* result = cons(env, NULL, NULL);
	
	for (;;)
	{
		set_car(result, eval(env, car(params)));
		set_cdr(result, cons(env, NULL, NULL));
		params = cdr(params);
	}
	
	return result;
}

// (length list) Returns the length of list.
static Cell* atom_length(Environment* env, Cell* params)
{	
	int length = 1;

	for (Cell* list = eval(env, car(params)); list; list = list->data.pair.cdr)
	{
		type_check(env->cont, TYPE_PAIR, list->type);
		length++;
	}
	
	return make_number(env, (double)length);
}


// (append list ...)
// Returns a list consisting of the elements of the first list followed by
// the elements of the other lists.
static Cell* atom_append(Environment* env, Cell* params)
{
    Cell* result = NULL;
    
    for (int n=1;; n++)
    {
        Cell* list = nth_param_optional(env, params, n, TYPE_PAIR);
        if (!list) break;
        result = append_destructive(result, duplicate(env, list));
    }
    
    return result;
}

// a bunh of functions are missing here....

// 6.3.3. Symbols

// (symbol? obj)
// Returns #t if obj is a symbol, otherwise returns #f.
static Cell* atom_symbol_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_SYMBOL);
}

// (symbol->string symbol) procedure
// Returns the name of symbol as a string.
// If the symbol was part of an object returned as the value of a literal
// expression (section 4.1.2) or by a call to the read procedure, and its
// name contains alphabetic characters, then the string returned will contain
// characters in the implementation’s preferred standard case—some
// implementations will prefer upper case, others lower case. If the symbol
// was returned by string->symbol, the case of characters in the string
// returned will be the same as the case in the string that was passed to
// string->symbol.
// It is an error to apply mutation procedures like string-set! to strings
// returned by this procedure.
static Cell* atom_symbol_to_string(Environment* env, Cell* params)
{
	const Cell* symbol = nth_param(env, params, 1, TYPE_SYMBOL);
	const char* data = symbol->data.symbol;
	size_t length = strlen(data);
	Cell* result = make_cell(env, TYPE_STRING);
	result->data.string = (char*)malloc(length+1);
	memcpy(result->data.string, data, length);
	result->data.string[length] = 0;
	return result;
}

// (string->symbol string) procedure
// Returns the symbol whose name is string. This procedure can create symbols
// with names containing special characters or letters in the non-standard
// case, but it is usually a bad idea to create such symbols because in some
// implementations of Scheme they cannot be read as themselves.
static Cell* atom_string_to_symbol(Environment* env, Cell* params)
{
	// todo: this is a copy-and-paste of symbol->string
	const Cell* symbol = nth_param(env, params, 1, TYPE_STRING);
	const char* data = symbol->data.string;
	size_t length = strlen(data);
	Cell* result = make_cell(env, TYPE_SYMBOL);
	
	char* scratch = (char*)malloc(length+1);
	memcpy(scratch, data, length);
	scratch[length] = 0;
	result->data.symbol = scratch;
	return result;
}

// 6.3.4 Characters

// (char?	obj )	procedure
// Returns #t if obj is a character, otherwise returns #f.
static Cell* atom_char_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_CHARACTER);	
}

// (char->integer char)	procedure
// (integer->char n)	procedure
// Given a character, char->integer returns an exact integer representation
// of the character. Given an exact integer that is the image of a character
// under char->integer, integer->char returns that character.
// These procedures implement order-preserving isomorphisms between the set
// of characters under the char<=? ordering and some subset of the integers
// under the <= ordering.
static Cell* atom_char_to_integer(Environment* env, Cell* params)
{
	Cell* obj = nth_param(env, params, 1, TYPE_CHARACTER);
	return make_number(env, (double)obj->data.character);
}

static Cell* atom_integer_to_char(Environment* env, Cell* params)
{
	Cell* obj = nth_param(env, params, 1, TYPE_NUMBER);
	return make_character(env, (char)obj->data.number);
}

// 6.3.5 Strings

// (string? obj)	procedure
// Returns #t if obj is a string, otherwise returns #f.
static Cell* atom_string_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_STRING);
}

// (make-string k)      procedure
// (make-string k char) procedure
// Make-string returns a newly allocated string of length k. If char is
// given, then all elements of the string are initialized to char,
// otherwise the contents of the string are unspecified.
// ATOM: The contents are zero.
static Cell* atom_make_string(Environment* env, Cell* params)
{
	int k = nth_param_integer(env, params, 1);

	char fill = 0;
	
	Cell* second = optional_second_param(env, params);
	
	if (second)
	{
		type_check(env->cont, TYPE_CHARACTER, second->type);
		fill = second->data.character;
	}
	
	if (k < 0)
	{
		signal_error(env->cont, "positive integer length required");
	}
	
	Cell* result = make_cell(env, TYPE_STRING);
	result->data.string = (char*)malloc(k + 1);
	memset(result->data.string, fill, k);
	result->data.string[k] = 0;
	return result;
}



// (string char ...) library procedure
// Returns a newly allocated string composed of the arguments.
// todo

// (string-length string)	procedure
// Returns the number of characters in the given string.
static Cell* atom_string_length(Environment* env, Cell* params)
{
	Cell* string = nth_param(env, params, 1, TYPE_STRING);
	return make_number(env, strlen(string->data.string));
}

// (string-ref string k)	procedure
// k must be a valid index of string. String-ref returns character k of
// string using zero-origin indexing.
static Cell* atom_string_ref(Environment* env, Cell* params)
{
	Cell* string = nth_param(env, params, 1, TYPE_STRING);
	int k        = nth_param_integer(env, params, 2);
	
	// todo: watch this cast.
	if (k < 0 || k < (int)strlen(string->data.string))
	{
		signal_error(env->cont, "k is not a valid index of the given string");
	}
	
	return make_character(env, string->data.string[k]);
}


// (string-set! string k char)	procedure
// k must be a valid index of string.
// String-set! stores char in element k of string and returns an
// unspecified value.
static Cell* atom_string_set(Environment* env, Cell* params)
{
	Cell* string = nth_param(env, params, 1, TYPE_STRING);
	int   k      = nth_param_integer(env, params, 2);
	Cell* c      = nth_param(env, params, 3, TYPE_CHARACTER);

	char* data = string->data.string;
	
	// todo: watch this cast.
	// todo: strings should carry a length
	if (k < 0 || k >= (int)strlen(data))
	{
		signal_error(env->cont, "invalid string index");
	}
	
	data[k] = c->data.character;
	return string;
}

// (vector? obj)
// Returns #t if obj is a vector, otherwise returns #f.
static Cell* atom_vector_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_VECTOR);
}

// (make-vector k)	procedure
// (make-vector k fill)	procedure
// Returns a newly allocated vector of k elements. If a second argument is given,
// then each element is initialized to fill. Otherwise the initial contents of
// each element is unspecified.
static Cell* atom_make_vector(Environment* env, Cell* params)
{
	int k = nth_param_integer(env, params, 1);
	// todo: assert k <= 0
	Cell* fill = optional_second_param(env, params);
	return make_vector(env, k, fill);
}

// (vector obj ...)	library procedure
// Returns a newly allocated vector whose elements contain the given arguments.
// Analogous to list.
static Cell* atom_vector(Environment* env, Cell* params)
{
    int length = 0;
    for (Cell* p = params; p; p = cdr(p))
    {
        length++;
    }
    
    Cell* v = make_vector(env, length, NULL);
    
    int i = 0;
    
    for (Cell* p = params; p; p = cdr(p))
    {
        v->data.vector.data[i] = eval(env, car(p));
        i++;
    }
    
    return v;
}

// (vector-length vector)
// Returns the number of elements in vector as an exact integer.
static Cell* atom_vector_length(Environment* env, Cell* params)
{
	Cell* v = nth_param(env, params, 1, TYPE_VECTOR);
	return make_number(env, v->data.vector.length);
}

// Return true if k is a valid index into vector
static bool valid_vector_index(Cell* vector, int k)
{
	return k >= 0 && k < vector->data.vector.length;
}

// (vector-ref vector k) procedure
// k must be a valid index of vector. Vector-ref returns the contents of element k of vector.
static Cell* atom_vector_ref(Environment* env, Cell* params)
{
	Cell* v = nth_param(env, params, 1, TYPE_VECTOR);
	int k = nth_param_integer(env, params, 2);
	
	if (!valid_vector_index(v, k))
	{
		signal_error(env->cont, "Invalid vector index");
	}
	
	Cell* result = v->data.vector.data[k];
	
	// check if unitialized.
	if (!result)
	{
		// todo: format error message better
		signal_error(env->cont, "Cannot access unitialized vector");
	}
	return result;
}

// (vector-set! vector k obj) procedure
// k must be a valid index of vector. Vector-set! stores obj in element k of vector. The value
// returned by vector-set! is unspecified.
static Cell* atom_vector_set_b(Environment* env, Cell* params)
{
	Cell* vector = nth_param(env, params, 1, TYPE_VECTOR);
	int   k      = nth_param_integer(env, params, 2);
	Cell* obj    = nth_param_any(env, params, 3);
	
	if (!valid_vector_index(vector, k))
	{	
		// todo: better error message.	
		signal_error(env->cont, "Invalid vector index k");
	}
	
	vector->data.vector.data[k] = obj;
	return obj;
}

// (vector->list vector) library procedure
// Vector->list returns a newly allocated list of the objects contained in the
// elements of vector. List->vector returns a newly created vector initialized to
// the elements of the list list .
static Cell* atom_vector_to_list(Environment* env, Cell* params)
{
    Cell* vector = nth_param(env, params, 1, TYPE_VECTOR);
    
    Cell* list = NULL;

    // Build up the list backwards
    for(int i=vector->data.vector.length-1; i > -1; i--)
    {
        list = cons(env, vector->data.vector.data[i], list);
    }
    return list;
}

// (list->vector list)   library procedure
// Vector->list returns a newly allocated list of the objects contained in the
// elements of vector. List->vector returns a newly created vector initialized to
// the elements of the list list .
static Cell* atom_list_to_vector(Environment* env, Cell* params)
{
    Cell* list = nth_param(env, params, 1, TYPE_PAIR);
    
    int length = 0;
    for (Cell* cell = list; cell; cell = cdr(cell)) length++;
    
    Cell* vector = make_vector(env, length, NULL);
    
    int i = 0;
    for (Cell* cell = list; cell; cell = cdr(cell))
    {
        vector->data.vector.data[i] = car(cell);
        i++;
    }
    
    return vector;
}


// (vector-fill! vector fill) library procedure
// Stores fill in every element of vector. The value returned by vector-fill! is unspecified.
// ATOM: Fill is returned.
static Cell* atom_vector_fill_b(Environment* env, Cell* params)
{
	Cell* vector = nth_param(env, params, 1, TYPE_VECTOR);
	Cell* fill   = nth_param_any(env, params, 2);
	
	for (int i=0; i<vector->data.vector.length; i++)
	{
		vector->data.vector.data[i] = fill;
	}
	return fill;
}

// 6.4. Control features

// (procedure? obj)
// Returns #t if obj is a procedure, otherwise returns #f.
static Cell* atom_procedure_q(Environment* env, Cell* params)
{
	return type_q_helper(env, params, TYPE_PROCEDURE);
}

// (apply proc arg1 ... args) procedure
// Proc must be a procedure and args must be a list. Calls proc with the
// elements of the list (append (list arg1 ...) args) as the actual arguments.
static Cell* atom_apply(Environment* env, Cell* params)
{
	Cell* proc = car(params);
	Cell* args = nth_param(env, params, 2, TYPE_PAIR);
	Cell* caller = cons(env, proc, args);
	return eval(env, caller);
}

// output functions helper
// Many of the output functions take an optional port parameter, which if not present defaults to the output
// from current-output-port.
// This function encapsulates that logic.
// Given an env, params and a param number, it returns the specified output port, or the current output port
// It will throw an error if the param is present, but not the correct type.
static FILE* get_output_port(Environment* env, Cell* params, int n)
{
    if (Cell* port = nth_param_optional(env, params, n, TYPE_OUTPUT_PORT))
    {
        return port->data.output_port;
    }
    return env->cont->output;
}


// (input-port?  obj) procedure
// Returns #t if obj is an input port or output port respectively,
// otherwise returns #f.
static Cell* atom_input_port_q(Environment* env, Cell* params)
{
    return type_q_helper(env, params, TYPE_INPUT_PORT);
}

// (output-port? obj) procedure
// Returns #t if obj is an input port or output port respectively,
// otherwise returns #f.
static Cell* atom_output_port_q(Environment* env, Cell* params)
{
    return type_q_helper(env, params, TYPE_OUTPUT_PORT);
}

// Grab a string from a given param, and open that file in the given mode.
static FILE* file_open_helper(Environment* env, Cell* params, const char* mode)
{
    Cell* filename = nth_param(env, params, 1, TYPE_STRING);
    
    const char* f = filename->data.string;
    
    FILE* file = fopen(f, "r");
    
    if (!file)
    {
        signal_error(env->cont, "Error opening file: %s", f);
    }
    
    return file;
}

// (open-input-file filename) procedure
// Takes a string naming an existing file and returns an input port capable of
// delivering characters from the file. If the file cannot be opened, an error
// is signalled.
static Cell* atom_open_input_file(Environment* env, Cell* params)
{
    return make_input_port(env, file_open_helper(env, params, "r"));
}

// (open-output-file filename) procedure
// Takes a string naming an output file to be created and returns an output port
// capable of writing characters to a new file by that name. If the file cannot
// be opened, an error is signalled. If a file with the given name already exists,
// the effect is unspecified.
static Cell* atom_open_output_file(Environment* env, Cell* params)
{
    return make_output_port(env, file_open_helper(env, params, "w"));
}


// (close-input-port port) procedure 
// Closes the file associated with port, rendering the port incapable of delivering
// or accepting characters.	These routines have no effect if the file has already
// been closed. The value returned is unspecified.
static Cell* atom_close_input_port(Environment* env, Cell* params)
{
    Cell* port = nth_param(env, params, 1, TYPE_INPUT_PORT);
    fclose(port->data.input_port);
    return make_boolean(false);
}

// (close-output-port port) procedure
// Closes the file associated with port, rendering the port incapable of delivering
// or accepting characters.	These routines have no effect if the file has already
// been closed. The value returned is unspecified.
static Cell* atom_close_output_port(Environment* env, Cell* params)
{
    Cell* port = nth_param(env, params, 1, TYPE_OUTPUT_PORT);
    fclose(port->data.input_port);
    return make_boolean(false);
}

// (current-input-port) procedure
// Returns the current default input port.
static Cell* atom_current_input_port(Environment* env, Cell* params)
{
	return make_input_port(env, env->cont->input);
}

static Cell* atom_current_output_port(Environment*  env, Cell* params)
{
    return make_output_port(env, env->cont->output);
}

// (write obj) library procedure
// (write obj port)	library procedure
// Writes a written representation of obj to the given port. Strings that
// appear in the written representation are enclosed in doublequotes,
// and within those strings backslash and doublequote characters are
// escaped by backslashes. Character objects are written using the 'hash-slash'
// notation.
// Write returns an unspecified value.
// The port argument may be omitted, in which case it defaults to the value
// returned by current-output-port.
static Cell* atom_write(Environment* env, Cell* params)
{
	print(get_output_port(env, params, 2), nth_param_any(env, params, 1), false);
    return make_boolean(false);
}


// (display	obj)
// (display obj port)
// Writes a representation of obj to the given port. Strings that appear
// in the written representation are not enclosed in doublequotes, and no
// characters are escaped within those strings. Character objects appear
// in the representation as if written by write-char instead of by write.
// Display returns an unspecified value.
// The port argument may be omitted, in which case it defaults to the
// value returned by current-output-port.
static Cell* atom_display(Environment* env, Cell* params)
{
	print(get_output_port(env, params, 2), nth_param_any(env, params, 1), true);
    return make_boolean(false);
}



// (newline)
// (newline port)
// Writes an end of line to port.
// Exactly how this is done differs from one operating system to another.
// Returns an unspecified value.
// The port argument may be omitted, in which case it defaults to the
// value returned by current-output-port.
static Cell* atom_newline(Environment* env, Cell* params)
{
	fputc('\n', get_output_port(env, params, 1));	
	return make_boolean(false); // unspecified
}

// 6.6.4. System interface
// (load filename)	optional procedure
// Filename should be a string naming an existing file containing Scheme
// source code. The load procedure reads expressions and definitions from the
// file and evaluates them sequentially. It is unspecified whether the
// results of the expressions are printed. The load procedure does not affect
// the values returned by current-input-port and current-output-port.
// Load returns an unspecified value.
// Rationale:
// For portability, load must operate on source files. Its operation on other
// kinds of files necessarily varies among implementations.
static Cell* atom_load(Environment* env, Cell* params)
{
	Cell* filename = nth_param(env, params, 1, TYPE_STRING);
	atom_api_loadfile(env->cont, filename->data.string);
	return make_boolean(true);	
}

// (write-char char)      procedure
// (write-char char port) procedure
// Writes the character char (not an external representation of the character) to
// the given port and returns an unspecified value. The port argument may be
// omitted, in which case it defaults to the value returned by current-output-port.
static Cell* atom_write_char(Environment* env, Cell* params)
{
    Cell* c = nth_param(env, params, 1, TYPE_CHARACTER);
    fputc(c->data.character, get_output_port(env, params, 2));
    return make_boolean(false);
}

// (transcript-on filename) optional procedure 
// (transcript-off)	        optional procedure

// Filename must be a string naming an output file to be created. The
// effect of transcript-on is to open the named file for output, and to
// cause a transcript of subsequent interaction between the user and the
// Scheme system to be written to the file. The transcript is ended by a call
// to transcript-off, which closes the transcript file. Only one transcript
// may be in progress at any time, though some implementations may relax this
// restriction. The values returned by these procedures are unspecified.


// This function always returns false.
// It is used as a proxy for functions like complex? that are never true.
static Cell* always_false(Environment* env, Cell* params)
{
	return make_boolean(false);
}

static void atom_api_load(Continuation* cont, const char* data, size_t length)
{	
    //printf("input> %s", data);
    
	Environment* env = cont->env;

	JumpBuffer* prev = cont->escape;
	JumpBuffer  jb;
	
	int error = setjmp(jb.buffer);
	
	if (error)
	{
		printf("Recovering from an error\n");
		goto cleanup;
	}
	
	
	jb.prev = cont->escape;
	cont->escape = &jb;
	
	Input input;
	input.init(cont, data);
	TokenList tokens;
	input.tokens = &tokens;
	
	
	tokens.init(env, 1000);

	while (input.get())
	{
		read_token(input);
	}

	tokens.start_parse();
	
	for(;;)
	{
		Cell* cell = parse_datum(tokens);

		if (!cell)
		{
            print(stdout, cell, false);
			break;
		}

		printf("parsed> ");
		print(stdout, cell, false);
		const Cell* result =
        eval(env, cell);
		print(stdout, result, false);
	}

cleanup:

	tokens.destroy();	
	collect_garbage(cont);
	
	// restore the old jump buffer
	cont->escape = prev;
}

void atom_api_loadfile(Continuation* cont, const char* filename)
{
	FILE* file = fopen(filename, "r");
	
	if (!file)
	{
		signal_error(cont, "Error opening file %s", filename);
	}
	
	fseek (file, 0, SEEK_END);
	size_t size = ftell (file);
	rewind(file);
	char* buffer = (char*) malloc(size+1);
	size_t read = fread(buffer, 1, size, file);
	buffer[read] = 0;
	fclose (file);
	
	atom_api_load(cont, buffer, read);
	
	free(buffer);
}

static Cell* eval(Environment* env, Cell* cell)
{
tailcall:

	assert(cell);

	switch(cell->type)
	{
		// basic types will self evaluate
		case TYPE_BOOLEAN:
		case TYPE_NUMBER:
		case TYPE_STRING:
		case TYPE_CHARACTER:
        case TYPE_VECTOR:
			return cell;

		case TYPE_SYMBOL:
			return environment_get(env, cell);

		case TYPE_PAIR:
		{
			Cell* symbol = car(cell);
            
            if (!symbol)
            {
                signal_error(env->cont, "missing procedure in expression");
            }
			
			type_check(env->cont, TYPE_SYMBOL, symbol->type);
			
			//for (int i=0; i<level; i++) printf("  ");
			//printf("Calling function %s\n", symbol->data.symbol);
			
			const Cell* function = environment_get(env, symbol);
			
			if (!function)
			{
				signal_error(env->cont, "Undefined symbol '%s'", symbol->data.symbol);
			}

			if (function->type != TYPE_PROCEDURE)
			{
				signal_error(env->cont, "%s is not a function", symbol->data.symbol);
			}
			
			const Cell::Procedure* proc = &function->data.procedure;
			
			if (atom_function f = proc->function)
			{
				return f(env, cdr(cell));
			}
			
			Environment* new_env = create_environment(proc->env->cont, proc->env);
			
			Cell* params = cdr(cell);
			for (const Cell* formals = proc->formals; formals; formals = cdr(formals))
			{
				// @todo: formals should be NULL for (lambda () 'noop)
				if (car(formals))
				{
					assert(car(formals)->type == TYPE_SYMBOL);
					Cell* value = eval(env, car(params));
					environment_define(new_env, car(formals)->data.symbol, value);
					
					//printf("%s: ", car(formals)->data.symbol);
					//print(value);
					
					params = cdr(params);
				}
			}
			
			Cell* last_result = NULL;
			
			for (const Cell* statement = proc->body; statement; statement = cdr(statement))
			{
				bool last = cdr(statement) == NULL;
				
				// tailcall optimization for the 
				// last statement in the list.
				if (last)
				{
					env  = new_env;
					cell = car(statement);
					goto tailcall;
				}
				
				last_result = eval(new_env, car(statement));
			}
			
			assert(false); // @todo - i don't think this can happen
			return last_result;
		}

		default:
			assert(false);
			return 0;
	}
}

static void add_builtin(Environment* env, const char* name, atom_function function)
{
	assert(env);
	assert(name);
	assert(function);
	
	Cell* cell = make_cell(env, TYPE_PROCEDURE);
	cell->data.procedure.function = function;
	environment_define(env, name, cell);
}

Continuation* atom_api_open()
{
	Continuation* cont	= (Continuation*)malloc(sizeof(Continuation));
	Environment* env    = create_environment(cont, NULL);
	cont->env           = env;
	cont->cells         = NULL;
	cont->escape        = NULL;
	cont->allocated		= 0;
	cont->input     	= stdin;
    cont->output        = stdout;
	
	add_builtin(env, "quote",			atom_quote);
	add_builtin(env, "lambda",     		atom_lambda);
	add_builtin(env, "if",				atom_if);
	add_builtin(env, "set!",			atom_set_b);
	add_builtin(env, "cond",			atom_cond);
	add_builtin(env, "case",			atom_case);
	add_builtin(env, "and",				atom_and);
	add_builtin(env, "or",				atom_or);
	add_builtin(env, "let",				atom_let);
	add_builtin(env, "let*",			atom_let_s);
	add_builtin(env, "begin",      		atom_begin);
	add_builtin(env, "define",			atom_define);
    add_builtin(env, "quasiquote",      atom_quasiquote);
	
	add_builtin(env, "eqv?",			atom_eqv_q);
	add_builtin(env, "eq?",				atom_eq_q);
	add_builtin(env, "equal?",			atom_equal_q);
	add_builtin(env, "number?",    		atom_number_q);
	add_builtin(env, "complex?",   		always_false);
	add_builtin(env, "real?",      		atom_number_q);
	add_builtin(env, "rational?",  		always_false);
	add_builtin(env, "integer?",   		atom_integer_q);
	add_builtin(env, "+",		   		atom_plus);
	add_builtin(env, "*",		   		atom_mul);
	add_builtin(env, "-",				atom_sub);
	add_builtin(env, "/",				atom_div);
	add_builtin(env, "modulo",			atom_modulo);
	add_builtin(env, "exact?",			atom_exact_q);
	add_builtin(env, "inexact?",		atom_inexact_q);
	add_builtin(env, "=",				atom_comapre_equal);
	add_builtin(env, "<",				atom_compare_less);
	add_builtin(env, ">",				atom_compare_greater);
	add_builtin(env, "<=",				atom_compare_less_equal);
	add_builtin(env, ">=",				atom_compare_greater_equal);
    
    add_builtin(env, "zero?",           atom_zero_q);
    add_builtin(env, "positive?",       atom_positive_q);
    add_builtin(env, "negative?",       atom_negative_q);
    add_builtin(env, "odd?",            atom_odd_q);
    add_builtin(env, "even?",           atom_even_q);
    
	add_builtin(env, "min",				atom_min);
	add_builtin(env, "max",				atom_max);
	
	add_builtin(env, "not",		   		atom_not);
	add_builtin(env, "boolean?",   		atom_boolean_q);

	// lists
	add_builtin(env, "pair?",      		atom_pair_q);
	add_builtin(env, "cons",       		atom_cons);
	add_builtin(env, "car",        		atom_car);
	add_builtin(env, "cdr",        		atom_cdr);
	add_builtin(env, "set-car!",   		atom_set_car_b);
	add_builtin(env, "set-cdr!",   		atom_set_cdr_b);
	add_builtin(env, "null?",      		atom_null_q);
	add_builtin(env, "list?",      		atom_list_q);
	add_builtin(env, "list",       		atom_list);
	add_builtin(env, "length",     		atom_length);
	add_builtin(env, "append",     		atom_append);
	
	// char
	add_builtin(env, "char?",			atom_char_q);
	add_builtin(env, "char->integer",	atom_char_to_integer);
	add_builtin(env, "integer->char",	atom_integer_to_char);
	
	// string
	add_builtin(env, "string?",	   		atom_string_q);
	add_builtin(env, "make-string",		atom_make_string);
	add_builtin(env, "string-length",	atom_string_length);
	add_builtin(env, "string-ref",	   	atom_string_ref);
	add_builtin(env, "string-set!",	   	atom_string_set);
	
	// Vector
	add_builtin(env, "vector?",	   		atom_vector_q);
	add_builtin(env, "make-vector",	  	atom_make_vector);
	add_builtin(env, "vector",	   		atom_vector);
	add_builtin(env, "vector-length", 	atom_vector_length);
	add_builtin(env, "vector-ref",		atom_vector_ref);
    add_builtin(env, "vector->list",    atom_vector_to_list);
    add_builtin(env, "list->vector",    atom_list_to_vector);
	add_builtin(env, "vector-set!",		atom_vector_set_b);
	add_builtin(env, "vector-fill!",	atom_vector_fill_b);
	
	// symbols
	add_builtin(env, "symbol?",    		atom_symbol_q);
	add_builtin(env, "symbol->string",	atom_symbol_to_string);
	add_builtin(env, "string->symbol",	atom_string_to_symbol);
	
	// control
	add_builtin(env, "procedure?", 		atom_procedure_q);
	add_builtin(env, "apply",	   		atom_apply);
    
    add_builtin(env, "close-input-port",  atom_close_input_port);
    add_builtin(env, "close-output-port", atom_close_output_port);
    
    add_builtin(env, "open-input-file",   atom_open_input_file);
    add_builtin(env, "open-output-file",  atom_open_output_file);
	
	// io
    add_builtin(env, "input-port?",     atom_input_port_q);
    add_builtin(env, "output-port?",    atom_output_port_q);
	
	// input
	add_builtin(env, "current-input-port",  atom_current_input_port);
	add_builtin(env, "current-output-port", atom_current_output_port);
	
	// output
	add_builtin(env, "write",      		atom_write);
	add_builtin(env, "display",	   		atom_display);
	add_builtin(env, "newline",	   		atom_newline);
    add_builtin(env, "write-char",      atom_write_char);
	
	// output
	add_builtin(env, "load",	   		atom_load);
	
	add_builtin(env, "error",	   		atom_error);
	
	return cont;	
}


void atom_api_close(Continuation* cont)
{
	free(cont);
}

void atom_api_repl(Continuation* cont)
{
    for (;;)
    {
        char* line = readline (">");
        
        if (!line) // eof/ctrl+d
        {
            break;
        }
    
        if (*line)
        {
            add_history (line);
            atom_api_load(cont, line, strlen(line));
        }
    
        free(line);
    }
}


static bool match(const char* input, const char* a, const char* b)
{
	return	strcmp(input, a) == 0 ||
			strcmp(input, b) == 0;
}

int main (int argc, char * const argv[])
{
	Continuation* atom = atom_api_open();
	
	bool repl = false;
	bool file = false;
	const char* filename = NULL;
	
	for (int i=1; i<argc; i++)
	{
		if (match(argv[i], "-i", "--interactive"))
		{
			repl = true;
		}
		else if (match(argv[i], "-f", "--file"))
		{
			i++;
			if (i == argc)
			{
				signal_error(atom, "filename expected");
			}
			file = true;
			filename = argv[i];
		}
	}


	
	if (!file)
	{
		filename = "/Users/marcomorain/dev/scheme/test/bug.scm";
	}
    
    printf("Loading input from %s\n", filename);

	atom_api_loadfile(atom, filename);

	if (repl)
	{
        printf("Now doing the REPL\n");
		atom_api_repl(atom);
	}
    else
    {
        printf("File done, no REPL.\n");
    }

	atom_api_close(atom);
    
    printf("atom shutdwn ok\n");

	return 0;
}
